#include "runtime/render/render_system.hpp"
#include "runtime/render/rvt/rvt_pass.hpp"
#include "runtime/render/vsm/virtual_shadow_cache.hpp"
#include "runtime/render/vsm/virtual_shadow_pass.hpp"

#include "runtime/font/font_system.hpp"
#include "runtime/performance/function_profile.hpp"
#include "runtime/performance/profiling.hpp"
#include "runtime/render/passes/meshlet_cull_pass.hpp"
#include "runtime/render/passes/planar_reflection_math.hpp"
#include "runtime/render/render_constants.hpp"
#include "runtime/render/render_context.hpp"
#include "runtime/render/render_extension.hpp"
#include "runtime/render/render_math.hpp"
#include "runtime/render/scene_gpu_resources.hpp"
#include "runtime/render/scene_render_query.hpp"
#include "runtime/resource/resource.hpp"
#include "runtime/scene/camera.hpp"
#include "runtime/scene/camera_math.hpp"
#include "runtime/scene/ecs_scene.hpp"
#include "runtime/scene/ecs_storage.hpp"
#include "runtime/scene/light.hpp"
#include "runtime/scene/mesh_renderer.hpp"
#include "runtime/scene/planar_reflection_component.hpp"
#include "runtime/scene/scene.hpp"
#include "runtime/scene/transform.hpp"
#include "runtime/utils/transform_math.hpp"
#include "runtime/world/world_system.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <bx/math.h>
#include <cmath>
#include <span>
#include <vector>

namespace engine
{
namespace
{
render_detail::PlanarReflectionScissor
reflection_scissor(const render_detail::PlanarReflectionPass& target,
                   const render_detail::RenderFrameData& frame)
{
    ENGINE_PROFILE_FUNCTION();
    const auto receiver =
        std::find_if(frame.meshes.begin(), frame.meshes.end(),
                     [&](const auto& mesh)
                     {
                         ENGINE_PROFILE_FUNCTION_PROBE();
                         return mesh.gpu->node_id == target.node_id;
                     });
    if (receiver == frame.meshes.end() || bgfx::isValid(receiver->gpu->deformed_vertices))
    {
        return {};
    }
    bool samples_reflection = false;
    float distortion = 0;
    for (const auto& material : receiver->gpu->materials)
    {
        if (!std::any_of(
                material.samplers.begin(), material.samplers.end(), [](const auto& sampler)
                {
                    ENGINE_PROFILE_FUNCTION_PROBE();
                    return sampler.source == schema::ShaderTextureSource::planar_reflection;
                }))
        {
            continue;
        }
        const auto* program = material.program;
        if (!program || program->path != "builtin://shaders/water")
        {
            return {};
        }
        const auto uniform =
            std::find_if(program->uniforms.begin(), program->uniforms.end(),
                         [](const auto& item)
                         {
                             ENGINE_PROFILE_FUNCTION_PROBE();
                             return item.name == "u_water_parameters";
                         });
        const auto index = static_cast<std::size_t>(uniform - program->uniforms.begin());
        if (uniform == program->uniforms.end() || index >= material.values.size() ||
            !std::isfinite(material.values[index][0]))
        {
            return {};
        }
        // fs_water adds a normalized normal's xy scaled by 0.025 and refractionStrength.
        distortion = std::max(distortion, 0.025002F * std::abs(material.values[index][0]));
        samples_reflection = true;
    }
    if (!samples_reflection)
    {
        return {};
    }
    return render_detail::planar_receiver_scissor(
        receiver->model.data(), target.view_projection.data(), receiver->gpu->minimum,
        receiver->gpu->maximum, distortion, target.width, target.height);
}
} // namespace

bool RenderSystem::draw_scene(Camera view_camera, const RenderSettings& rendering,
                              render_detail::RenderGraph* hdr_scheduler)
{
    ENGINE_PROFILE_ZONE("Render.drawScene");
    if (hdr_capture_status_ == SceneCaptureStatus::pending &&
        (!world_ || !world_->scene() || world_->scene()->id() != hdr_capture_scene_id_ ||
         ((!scene_resources_ || !scene_resources_->hdr_capture_pending) &&
          !render_detail::drawable_scene(*world_, view_camera, !extensions_.empty()))))
    {
        cancel_hdr_capture();
    }
    if (capture_status_ == SceneCaptureStatus::pending && !capture_unchanged())
    {
        cancel_diffuse_capture();
    }
    // Disabling a sky-only scene makes it non-drawable. Retire its LUTs before that early return.
    if (scene_resources_ &&
        (!rendering.atmosphere.atmosphere_enabled || !rendering.atmosphere_sun_valid))
    {
        scene_resources_->atmosphere.release();
    }
    const Scene* scene = world_ ? world_->scene() : nullptr;
    if (!scene || !scene_resources_ ||
        !render_detail::drawable_scene(*world_, view_camera, !extensions_.empty()))
    {
        release_vsm();
        scene_collected_ = scene != nullptr && !render_detail::drawable_scene(*world_, view_camera,
                                                                              !extensions_.empty());
        return true;
    }
    const Camera camera = view_camera ? view_camera : scene->active_camera();
    double absolute_camera[16]{};
    float camera_world[16]{}, view[16]{}, projection[16]{};
    const auto camera_transform = camera.node().get_component<Transform>();
    CameraSettings settings;
    const float aspect = static_cast<float>(width_) / static_cast<float>(height_);
    if (!camera.get_settings(settings) || !camera_transform ||
        !camera_transform.world_matrix(absolute_camera))
    {
        return false;
    }
    const Vec3 origin{absolute_camera[12], absolute_camera[13], absolute_camera[14]};
    transform_math::relative(absolute_camera, origin, camera_world);
    if (!camera_detail::view_matrix(camera_world, view) ||
        !camera_detail::projection_matrix(settings, aspect, bgfx::getCaps()->homogeneousDepth,
                                          projection))
    {
        return false;
    }
    // Mirrors and light capture need the actual orthonormal camera, not a scaled Node transform.
    bx::mtxInverse(camera_world, view);
    auto& resources = *scene_resources_;
    resources.select_targets(render_detail::target_scope(*scene));
    if (resources.targets().stage != 10)
    {
        return true;
    }
    if (!resources.collect_frame(*world_, origin, resources.frame_data))
    {
        return false;
    }
    scene_collected_ = resources.frame_complete;
    render_detail::SceneDrawView rvt_view;
    rvt_view.world_origin = origin;
    rvt_view.world = camera_world;
    rvt_view.view = view;
    rvt_view.projection = projection;
    rvt_view.width = static_cast<std::uint16_t>(width_);
    rvt_view.height = static_cast<std::uint16_t>(height_);
    rvt_view.lod_observer = {camera.instance_id(), scene->id()};
    prepare_rvt(rendering, resources.frame_data, rvt_view);
    for (std::size_t index = 0; index < resources.targets().planar.size(); ++index)
    {
        auto& target = resources.targets().planar[index];
        if (target.node_id == 0)
        {
            continue;
        }
        ENGINE_PROFILE_ZONE("Render.planarViewSubmit");
        const auto node = world_->find_node(target.node_id);
        const auto base = static_cast<bgfx::ViewId>(3 + index * 39);
        render_detail::SceneDrawView draw;
        draw.renderer = this;
        draw.world_origin = origin;
        draw.rvt = rvt_status_.effective ? rvt_pass_.get() : nullptr;
        draw.id = static_cast<bgfx::ViewId>(base + 30);
        draw.width = target.width;
        draw.height = target.height;
        draw.world = target.world.data();
        draw.view = target.view.data();
        draw.projection = target.projection.data();
        const auto scissor = reflection_scissor(target, resources.frame_data);
        const auto cropped_projection =
            scissor.width != 0 ? render_detail::planar_culling_projection(
                                     target.projection, scissor, draw.width, draw.height)
                               : std::array<float, 16>{};
        if (scissor.width != 0)
        {
            draw.culling_projection = cropped_projection.data();
        }
        ENGINE_PROFILE_PLOT("Render.planarCapturePixels",
                            std::int64_t{scissor.width != 0 ? scissor.width : draw.width} *
                                (scissor.width != 0 ? scissor.height : draw.height));
        draw.reflection = true;
        draw.frustum_culling_enabled = settings.frustum_culling_enabled;
        draw.lod_observer = {camera.instance_id(),
                             node.get_component<PlanarReflectionComponent>().instance_id()};
        draw.multisampling = config_.msaa_samples > 1;
        draw.max_anisotropy = config_.max_anisotropy;
        render_detail::ShadowFrame shadow;
        if (!resources.prepare_lighting(resources.frame_data, rendering, config_, draw, settings,
                                        shadow, index, base, nullptr, nullptr))
        {
            return false;
        }
        auto& list = resources.draw_lists[index];
        if (!resources.collect_view(resources.frame_data, draw, list))
        {
            return false;
        }
        const auto color_view = draw.id;
        const auto depth_view = render_detail::planar_depth_views[index];
        draw.opaque_depth_prepared =
            draw.multisampling &&
            render_detail::can_submit_opaque_depth(resources.frame_data, list) &&
            target.begin_opaque_depth(depth_view, scissor);
        if (draw.opaque_depth_prepared)
        {
            ENGINE_PROFILE_ZONE("Render.planarDepthSubmit");
            draw.id = depth_view;
            if (!resources.draw_meshes(resources.frame_data, list, rendering, draw,
                                       render_detail::SceneDrawMode::depth))
            {
                return false;
            }
        }
        draw.id = color_view;
        target.begin(draw.id, rendering.clear_color, draw.opaque_depth_prepared, scissor);
        if (!resources.atmosphere.draw(scene->id(), rendering, draw, index,
                                       static_cast<bgfx::ViewId>(base + 29)))
        {
            return false;
        }
        if (!resources.draw_meshes(resources.frame_data, list, rendering, draw,
                                   render_detail::SceneDrawMode::opaque))
        {
            return false;
        }
    }
    render_detail::SceneDrawView draw;
    draw.renderer = this;
    draw.world_origin = origin;
    draw.rvt = rvt_status_.effective ? rvt_pass_.get() : nullptr;
    draw.width = static_cast<std::uint16_t>(width_);
    draw.height = static_cast<std::uint16_t>(height_);
    draw.world = camera_world;
    draw.view = view;
    draw.projection = projection;
    draw.visibility = camera;
    draw.frustum_culling_enabled = settings.frustum_culling_enabled;
    draw.lod_observer = {camera.instance_id(), world_->scene() ? world_->scene()->id() : 0};
    draw.max_anisotropy = config_.max_anisotropy;
    render_detail::VsmPageBatch vsm_batch;
    if (!prepare_vsm(resources.frame_data, draw, settings, vsm_batch))
    {
        return false;
    }
    render_detail::ShadowFrame shadow;
    if (!resources.prepare_lighting(resources.frame_data, rendering, config_, draw, settings,
                                    shadow, 2, render_detail::shadow_view, nullptr,
                                    vsm_batch.generation ? &vsm_batch : nullptr))
    {
        if (vsm_cache_)
        {
            vsm_cache_->cancel_batch();
        }
        if (vsm_batch.generation)
        {
            vsm_status_.state = VsmState::failed;
            vsm_status_.error_code = "vsm_lighting_failed";
            vsm_status_.error = "Cannot upload the virtual shadow page table";
        }
        return false;
    }
    if (vsm_batch.generation)
    {
        const auto& shadow_pass = resources.targets().shadow;
        draw.vsm.local_range_layout = vsm_pass_->submit_local_depth_ranges(
            shadow_pass.texture(), shadow.local_records, shadow_pass.local_depth_revision());
        if (!vsm_cache_->publish(vsm_batch, submission_sequence_ + 1))
        {
            return false;
        }
        (void)vsm_cache_->status(vsm_status_);
        vsm_status_.resident_bytes = draw.vsm.resident_bytes + vsm_batch.page_table.size() * 16;
    }
    if (!resources.collect_view(resources.frame_data, draw, resources.draw_lists[2]))
    {
        return false;
    }
    if (rendering.meshlet_culling_enabled && render_detail::meshlet_supported())
    {
        if (meshlet_culling_ &&
            !meshlet_culling_->dispatch(
                resources.frame_data, draw, resources.draw_lists[2],
                debug_requests_[static_cast<std::size_t>(RenderFeature::meshlet_statistics)]))
        {
            return false;
        }
    }
    else if (meshlet_culling_ &&
             meshlet_culling_->statistics().state != MeshletSampleState::disabled)
    {
        meshlet_culling_->reset_scene();
    }
    auto& surface = resources.targets().surface_pass;
    auto& list = resources.draw_lists[2];
    draw.surface_writes_depth = surface.can_combine_depth(resources.frame_data, list);
    ENGINE_PROFILE_PLOT("Render.sceneSurfaceCombined",
                        static_cast<std::int64_t>(draw.surface_writes_depth));
    surface.begin(view, projection, draw.surface_writes_depth);
    const auto submit_surface = [&]
    {
        ENGINE_PROFILE_ZONE("Render.surfaceSubmit");
        draw.id = render_detail::surface_view;
        return resources.draw_meshes(resources.frame_data, list, rendering, draw,
                                     render_detail::SceneDrawMode::surface);
    };
    {
        ENGINE_PROFILE_ZONE("Render.depthSubmit");
        if (draw.surface_writes_depth)
        {
            if (!submit_surface())
            {
                return false;
            }
        }
        else
        {
            draw.id = render_detail::ambient_occlusion_depth_view;
            if (!resources.draw_meshes(resources.frame_data, list, rendering, draw,
                                       render_detail::SceneDrawMode::depth))
            {
                return false;
            }
        }
    }
    if (rendering.ssr_enabled && !draw.surface_writes_depth && !submit_surface())
    {
        return false;
    }
    const auto depth_revision =
        (rendering.ao_enabled || rendering.ssr_enabled || vsm_batch.generation)
            ? surface.identify_depth(draw, resources.frame_data, list)
            : 0;
    if (vsm_batch.generation &&
        !vsm_pass_->submit_feedback(vsm_batch.view, surface.depth(), surface.depth_inputs(),
                                    submission_sequence_ + 1))
    {
        return false;
    }
    if (rendering.ao_enabled)
    {
        ENGINE_PROFILE_ZONE("Render.aoSubmit");
        if (!resources.targets().ambient_occlusion.compute(settings, rendering, projection,
                                                           surface.depth(), depth_revision))
        {
            return false;
        }
    }
    draw.multisampling = config_.msaa_samples > 1;
    auto& hdr = resources.targets().hdr_post_process;
    draw.opaque_depth_prepared =
        draw.multisampling && render_detail::can_submit_opaque_depth(resources.frame_data, list) &&
        hdr.begin_opaque_depth(view, projection);
    ENGINE_PROFILE_PLOT("Render.opaqueDepthReuse",
                        static_cast<std::int64_t>(draw.opaque_depth_prepared));
    if (draw.opaque_depth_prepared)
    {
        ENGINE_PROFILE_ZONE("Render.opaqueDepthSubmit");
        draw.id = render_detail::opaque_depth_view;
        if (!resources.draw_meshes(resources.frame_data, list, rendering, draw,
                                   render_detail::SceneDrawMode::depth))
        {
            return false;
        }
    }
    draw.id = render_detail::main_view;
    draw.ambient_occlusion = rendering.ao_enabled;
    bgfx::setViewTransform(draw.id, view, projection);
    hdr.begin_scene(rendering.clear_color, draw.opaque_depth_prepared);
    if (!resources.atmosphere.draw(scene->id(), rendering, draw, 2,
                                   render_detail::atmosphere_main_skyview))
    {
        return false;
    }
    {
        ENGINE_PROFILE_ZONE("Render.opaqueSubmit");
        if (!resources.draw_meshes(resources.frame_data, resources.draw_lists[2], rendering, draw,
                                   render_detail::SceneDrawMode::opaque))
        {
            return false;
        }
    }
    auto frozen_scene = resources.targets().hdr_post_process.opaque_color();
    if (rendering.ssr_enabled)
    {
        ENGINE_PROFILE_ZONE("Render.ssrSubmit");
        // Match the main PBR pass: disabling IBL selects the analytical DFG
        // fallback even when an earlier bake has left its LUT loaded.
        const bgfx::TextureHandle reflection_lut =
            rendering.ibl_enabled && resources.targets().ibl_textures[2]
                ? resources.targets().ibl_textures[2]->handle
                : bgfx::TextureHandle{bgfx::kInvalidHandle};
        if (!resources.targets().reflections.render(
                view, projection, frozen_scene,
                resources.targets().hdr_post_process.opaque_specular(), surface.depth(),
                surface.normal_roughness(), surface.specular(), reflection_lut, rendering,
                depth_revision))
        {
            return false;
        }
        frozen_scene = resources.targets().reflections.texture();
    }
    resources.targets().hdr_post_process.begin_transparency(frozen_scene, view, projection);
    draw.id = render_detail::transparent_view;
    draw.textures[0] = frozen_scene;
    draw.textures[1] = surface.depth();
    {
        ENGINE_PROFILE_ZONE("Render.transparentSubmit");
        if (!resources.draw_meshes(resources.frame_data, resources.draw_lists[2], rendering, draw,
                                   render_detail::SceneDrawMode::transparent))
        {
            return false;
        }
    }
    if (fonts_)
    {
        for (const auto& owner : world_->scenes())
        {
            if (!fonts_->render_scene(*owner, origin, render_detail::transparent_view, true))
            {
                return false;
            }
        }
    }
    if (!extensions_.empty())
    {
        ENGINE_PROFILE_ZONE("Render.extensionsSubmit");
        RenderContext context{*scene};
        context.world_origin = origin;
        std::copy_n(view, context.view.size(), context.view.begin());
        std::copy_n(projection, context.projection.size(), context.projection.begin());
        context.view_id = render_detail::extension_view;
        context.width = draw.width;
        context.height = draw.height;
        context.hdr = true;
        bgfx::setViewName(context.view_id, "Scene extensions");
        bgfx::setViewFrameBuffer(context.view_id,
                                 resources.targets().hdr_post_process.scene_framebuffer());
        bgfx::setViewRect(context.view_id, 0, 0, context.width, context.height);
        bgfx::setViewClear(context.view_id, BGFX_CLEAR_NONE);
        bgfx::setViewMode(context.view_id, bgfx::ViewMode::Sequential);
        bgfx::setViewTransform(context.view_id, view, projection);
        invoking_extensions_ = true;
        for (auto* extension : extensions_)
        {
            if (!extension->draw(context))
            {
                invoking_extensions_ = false;
                return false;
            }
        }
        invoking_extensions_ = false;
    }
    {
        ENGINE_PROFILE_ZONE("Render.debugOverlay");
        if (!draw_debug_overlay(camera, view, projection))
        {
            return false;
        }
    }
    if (!resources.targets().hdr_post_process.resolve(rendering, *frame_output_, hdr_scheduler))
    {
        return false;
    }
    submit_hdr_capture();
    return true;
}
} // namespace engine
