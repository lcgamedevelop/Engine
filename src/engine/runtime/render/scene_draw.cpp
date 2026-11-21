#include "runtime/render/scene_draw.hpp"
#include "runtime/render/rvt/rvt_pass.hpp"

#include "runtime/performance/function_profile.hpp"
#include "runtime/performance/profiling.hpp"
#include "runtime/render/render_constants.hpp"
#include "runtime/render/render_math.hpp"
#include "runtime/render/scene_gpu_resources.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <bx/math.h>
#include <cmath>
#include <cstring>
#include <map>

namespace engine::render_detail
{
namespace
{
constexpr std::uint16_t instance_stride = 7 * 4 * sizeof(float);
}

bool can_submit_opaque_depth(const RenderFrameData& frame_data, const ViewDrawList& list)
{
    ENGINE_PROFILE_FUNCTION();
    if (list.opaque.empty())
    {
        return false;
    }
    for (const auto& item : list.opaque)
    {
        const auto& material = frame_data.meshes[item.mesh].gpu->materials[item.slot];
        if (material.frame_textures || !bgfx::isValid(material.program->depth_program))
        {
            return false;
        }
    }

    std::size_t instance_count = 0;
    for (const auto& batch : list.batches)
    {
        if (batch.items.size() < 2)
        {
            continue;
        }
        const auto& item = list.opaque[batch.items.front()];
        const auto* program = frame_data.meshes[item.mesh].gpu->materials[item.slot].program;
        if (!bgfx::isValid(program->instanced_depth_program))
        {
            return false;
        }
        if (!bgfx::isValid(batch.meshlets.commands))
        {
            instance_count += batch.items.size();
        }
    }
    if (instance_count > UINT32_MAX / (2 * instance_stride))
    {
        return false;
    }
    // With enough space for both passes the same batches use identical instanced vertices.
    const auto instances = static_cast<std::uint32_t>(instance_count * 2);
    return instances == 0 ||
           bgfx::getAvailInstanceDataBuffer(instances, instance_stride) == instances;
}

bool prepare_draw_batches(const RenderFrameData& frame_data, const ViewDrawList& list,
                          const SceneDrawView& draw, SceneDrawMode mode,
                          std::vector<DrawBatch>& batches)
{
    ENGINE_PROFILE_FUNCTION();
    const bool transparent = mode == SceneDrawMode::transparent;
    const bool depth_only = mode == SceneDrawMode::depth;
    const bool capture = mode == SceneDrawMode::capture;
    const bool instancing = bgfx::getCaps()->limits.maxInstanceData >= 7;
    const auto& items = transparent ? list.transparent : list.opaque;
    std::map<render_detail::DrawBatchKey, std::size_t> batch_index;
    batches.clear();
    {
        ENGINE_PROFILE_ZONE("Render.buildBatches");
        for (std::size_t index = 0; index < items.size(); ++index)
        {
            const auto& item = items[index];
            const auto& mesh = frame_data.meshes[item.mesh];
            const auto& material = mesh.gpu->materials[item.slot];
            const auto* program = material.program;
            if (!program)
            {
                return false;
            }
            if (mode == SceneDrawMode::surface && !bgfx::isValid(program->surface_program))
            {
                continue;
            }
            auto cull = capture ? std::uint64_t{0}
                                : render_detail::cull_state(material.cull, mesh.model.data());
            if (draw.reflection)
            {
                if (cull == BGFX_STATE_CULL_CW)
                {
                    cull = BGFX_STATE_CULL_CCW;
                }
                else if (cull == BGFX_STATE_CULL_CCW)
                {
                    cull = BGFX_STATE_CULL_CW;
                }
            }
            auto state = cull | BGFX_STATE_DEPTH_TEST_LESS;
            if (!transparent)
            {
                state |= BGFX_STATE_WRITE_Z;
            }
            if (!depth_only)
            {
                state |= BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A;
            }
            if (mode == SceneDrawMode::surface)
            {
                state =
                    cull | BGFX_STATE_DEPTH_TEST_EQUAL | BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A;
            }
            if (transparent)
            {
                const auto alpha_source =
                    material.unlit ? BGFX_STATE_BLEND_ZERO : BGFX_STATE_BLEND_ONE;
                state |= material.blend == schema::BlendMode::premultiplied
                             ? BGFX_STATE_BLEND_FUNC_SEPARATE(
                                   BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_INV_SRC_ALPHA,
                                   alpha_source, BGFX_STATE_BLEND_INV_SRC_ALPHA)
                             : BGFX_STATE_BLEND_FUNC_SEPARATE(
                                   BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_INV_SRC_ALPHA,
                                   alpha_source, BGFX_STATE_BLEND_INV_SRC_ALPHA);
            }
            if (draw.multisampling)
            {
                state |= BGFX_STATE_MSAA;
            }
            render_detail::DrawBatchKey key;
            key.rvt_role = mesh.rvt_role;
            key.geometry = reinterpret_cast<std::uintptr_t>(item.geometry);
            key.state = state;
            key.binding = material.binding_id;
            key.lod = item.lod;
            key.first = item.first;
            key.count = item.count;
            const auto instanced_color = mode == SceneDrawMode::opaque && material.pbr_opaque
                                             ? program->instanced_pbr_opaque_program
                                             : program->instanced_program;
            key.program = (mode == SceneDrawMode::surface ? program->instanced_surface_program
                                                          : instanced_color)
                              .idx;
            key.lightmap = item.lightmap.idx;
            key.has_lightmap = item.has_lightmap;
            key.has_probe = item.has_probe;
            if (item.has_probe)
            {
                key.coefficients = item.coefficients;
            }
            const bool eligible = instancing && mesh.instancing && mesh.instance_eligible &&
                                  !mesh.plane && !material.frame_textures && !transparent &&
                                  !capture && item.geometry == mesh.gpu->geometry.get() &&
                                  key.program != bgfx::kInvalidHandle;
            if (eligible)
            {
                auto [position, inserted] = batch_index.try_emplace(key, batches.size());
                if (inserted)
                {
                    batches.push_back({key, {}, 0, {}});
                }
                batches[position->second].items.push_back(index);
            }
            else
            {
                batches.push_back({key, {index}, 0, {}});
            }
        }
    }
    return true;
}
} // namespace engine::render_detail

namespace engine
{
bool RenderSystem::SceneGpuResources::prepare_lighting(
    const render_detail::RenderFrameData& frame_data, const RenderSettings& rendering,
    const RenderConfig& config, render_detail::SceneDrawView& draw, const CameraSettings& lens,
    render_detail::ShadowFrame& frame, std::size_t cluster_index, bgfx::ViewId base,
    const SceneCaptureRequest* capture, const render_detail::VsmPageBatch* vsm)
{
    ENGINE_PROFILE_FUNCTION();
    const auto fail = [](const char* message)
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Lighting preparation failed: %s", message);
        return false;
    };
    std::vector<render_detail::ClusterLight> lights;
    std::vector<render_detail::LocalShadowLight> local_lights;
    std::uint32_t directional_count = 0;
    std::uint32_t shadow_light = UINT32_MAX;
    auto light_settings = rendering;
    light_settings.directional_shadows = false;
    for (const auto& source : frame_data.lights)
    {
        if (capture && std::find(capture->light_node_ids.begin(), capture->light_node_ids.end(),
                                 source.node_id) == capture->light_node_ids.end())
        {
            continue;
        }
        if (lights.size() >= config.maximum_lights)
        {
            return fail("Light count exceeds the configured limit");
        }
        const auto& matrix = source.world;
        const float length = std::hypot(matrix[8], matrix[9], matrix[10]);
        if (!(length > 0))
        {
            return fail("Light direction is degenerate");
        }
        const auto& lighting = source.settings;
        constexpr float radians = 0.01745329251994329577F;
        const auto index = static_cast<std::uint32_t>(lights.size());
        render_detail::ClusterLight packed;
        packed.position_range = {matrix[12], matrix[13], matrix[14], lighting.range};
        packed.direction_outer = {-matrix[8] / length, -matrix[9] / length, -matrix[10] / length,
                                  std::cos(lighting.outer_angle * radians)};
        packed.color_intensity = {lighting.color.r(), lighting.color.g(), lighting.color.b(),
                                  lighting.intensity};
        packed.kind_inner = {static_cast<float>(lighting.kind),
                             std::cos(lighting.inner_angle * radians), 0, 0};
        lights.push_back(packed);
        const bool directional = lighting.kind == schema::LightKind::directional;
        directional_count += directional;
        if (lighting.cast_shadows)
        {
            if (directional)
            {
                if (shadow_light != UINT32_MAX)
                {
                    return fail("More than one directional light requests shadows");
                }
                shadow_light = index;
                light_settings.directional_shadows = true;
                light_settings.shadow_resolution =
                    std::min(lighting.shadow_resolution, config.shadow_atlas_size);
                light_settings.shadow_bias = lighting.shadow_bias;
                light_settings.shadow_cascades = lighting.shadow_cascades;
                light_settings.shadow_distance = lighting.shadow_distance;
                light_settings.shadow_split_lambda = lighting.shadow_split_lambda;
                light_settings.shadow_cascade_blend = lighting.shadow_cascade_blend;
            }
            else
            {
                local_lights.push_back(source);
                local_lights.back().light_index = index;
            }
        }
    }
    for (auto& matrix : frame.matrices)
    {
        bx::mtxIdentity(matrix.data());
    }
    if (capture)
    {
        // Bake every selected surface, even beyond the gameplay light's fade range.
        // Tighten the request frustum to its actual contributors instead of stretching
        // a targets().shadow cascade to an arbitrary hemicube far plane (often 10000 metres).
        float farthest = lens.near_clip;
        for (const auto id : capture->node_ids)
        {
            const auto found =
                std::find_if(frame_data.meshes.begin(), frame_data.meshes.end(),
                             [&](const auto& mesh)
                             {
                                 ENGINE_PROFILE_FUNCTION_PROBE();
                                 return mesh.gpu->node_id == id;
                             });
            if (found == frame_data.meshes.end())
            {
                return fail("A capture mesh is absent from the prepared scene");
            }
            const auto* mesh = found->gpu;
            const auto& model = found->model;
            for (unsigned corner = 0; corner < 8; ++corner)
            {
                std::array<float, 3> point{};
                for (std::size_t axis = 0; axis < 3; ++axis)
                {
                    point[axis] =
                        (corner & (1U << axis)) ? mesh->maximum[axis] : mesh->minimum[axis];
                }
                point = render_detail::transform_point(model.data(), point);
                point = render_detail::transform_point(draw.view, point);
                farthest = std::max(farthest, -point[2]);
            }
        }
        light_settings.shadow_distance = std::min(lens.far_clip, farthest);
    }
    const bool csm = light_settings.directional_shadows && shadow_light < lights.size() &&
                     std::min(lens.far_clip, light_settings.shadow_distance) > lens.near_clip;
    const float* receiver_projection =
        draw.culling_projection ? draw.culling_projection : draw.projection;
    if (csm && !render_detail::prepare_shadow_frame(
                   lens, light_settings, static_cast<float>(draw.width) / draw.height, draw.world,
                   draw.view, receiver_projection, lights[shadow_light].direction_outer, frame))
    {
        return fail("Cannot construct directional shadow cascades");
    }
    std::span<const std::uint64_t> caster_ids;
    if (capture)
    {
        caster_ids = capture->node_ids;
    }
    render_detail::ShadowAtlasView atlas_view;
    if (!capture)
    {
        for (std::size_t index = 0; index < targets().planar.size(); ++index)
        {
            if (targets().planar[index].node_id != 0)
            {
                if (draw.reflection && index == cluster_index)
                {
                    atlas_view.index = atlas_view.count;
                }
                ++atlas_view.count;
            }
        }
    }
    if (!targets().shadow.draw(frame_data, light_settings, frame, shadow_fallback, config,
                               local_lights, lights, draw.world, base, atlas_view,
                               capture ? &caster_ids : nullptr))
    {
        return fail("Cannot submit the shadow atlas");
    }
    auto& cluster = targets().clusters[cluster_index];
    if (!cluster.prepare(config, draw.width, draw.height,
                         vsm ? static_cast<std::uint32_t>(vsm->page_table.size()) : 0))
    {
        return fail("Cannot prepare clustered lighting buffers");
    }
    if (!cluster.cull(static_cast<bgfx::ViewId>(base + 28), lens, draw.view, draw.projection,
                      lights, directional_count, frame.local_records, vsm))
    {
        return fail("Cannot submit clustered light culling");
    }
    draw.cluster = &cluster;
    draw.shadow = &frame;
    draw.shadow_light = shadow_light;
    draw.shadow_bias = light_settings.shadow_bias;
    draw.inverse_shadow_size = 1.0F / config.shadow_atlas_size;
    draw.near_clip = lens.near_clip;
    return true;
}

bool RenderSystem::SceneGpuResources::draw_meshes(const render_detail::RenderFrameData& frame_data,
                                                  render_detail::ViewDrawList& list,
                                                  const RenderSettings& settings,
                                                  const render_detail::SceneDrawView& draw,
                                                  render_detail::SceneDrawMode mode)
{
    using render_detail::SceneDrawMode;
    ENGINE_PROFILE_ZONE("Render.submitMeshes");
    RenderViewStats stats =
        mode == SceneDrawMode::transparent ? list.transparent_stats : list.opaque_stats;
    if (mode != SceneDrawMode::opaque || draw.reflection)
    {
        stats.meshlet_candidate_pairs = 0;
        stats.meshlet_candidate_triangles = 0;
        stats.meshlet_missing_data = 0;
        stats.meshlet_incompatible_draws = 0;
        stats.meshlet_budget_fallbacks = 0;
        stats.meshlet_instanced_batches = 0;
        stats.meshlet_gpu = {};
    }
    stats.view_id = draw.id;
    const auto frame = draw.renderer ? draw.renderer->frame_number() : 0;
    if (frame_time_stamp != frame)
    {
        frame_time_stamp = frame;
        frame_time = {static_cast<float>(static_cast<double>(SDL_GetTicks()) / 1000.0), 0, 0, 0};
        bgfx::setFrameUniform(frame_time_uniform, frame_time.data());
    }
    const bool depth_only = mode == SceneDrawMode::depth;
    const bool capture = mode == SceneDrawMode::capture;
    const bool transparent = mode == SceneDrawMode::transparent;
    const bool surface_writes_depth = mode == SceneDrawMode::surface && draw.surface_writes_depth;
    const bool ibl_ready =
        settings.ibl_enabled &&
        std::all_of(targets().ibl_textures.begin(), targets().ibl_textures.end(),
                    [](const auto* texture)
                    {
                        ENGINE_PROFILE_FUNCTION_PROBE();
                        return texture && bgfx::isValid(texture->handle);
                    });
    const float camera_position_value[]{draw.world[12], draw.world[13], draw.world[14], 1};
    const float ambient_value[]{settings.ambient_color[0], settings.ambient_color[1],
                                settings.ambient_color[2], settings.ambient_intensity};
    const float sky[]{settings.ambient_sky_color[0], settings.ambient_sky_color[1],
                      settings.ambient_sky_color[2], settings.ambient_tricolor ? 1.0F : 0.0F};
    const float equator[]{settings.ambient_equator_color[0], settings.ambient_equator_color[1],
                          settings.ambient_equator_color[2], 0};
    const float ground[]{settings.ambient_ground_color[0], settings.ambient_ground_color[1],
                         settings.ambient_ground_color[2], 0};
    const float fog[]{settings.fog_color[0], settings.fog_color[1], settings.fog_color[2],
                      static_cast<float>(settings.fog_mode)};
    const float fog_range_value[]{settings.fog_start, settings.fog_end, settings.fog_density,
                                  draw.near_clip};
    const float tone[]{settings.exposure, settings.filmic_tonemap ? 1.0F : 0.0F, 1, 0};
    const float ibl[]{
        ibl_ready ? 1.0F : 0.0F, settings.ibl_intensity,
        ibl_ready ? static_cast<float>(
                        targets().ibl_textures[1]->resource.data<TextureResource>()->mip_count - 1)
                  : 0,
        0};
    const float shadow_values[]{draw.shadow_bias, draw.inverse_shadow_size,
                                static_cast<float>(draw.shadow_light),
                                draw.shadow->tile_size ? 1.0F : 0.0F};
    const float capture_value[]{capture ? 1.0F : 0.0F, 0, 0, 0};
    const float zero[4]{};
    const float frame_value[]{1.0F / draw.width, 1.0F / draw.height, frame_time[0],
                              bgfx::getCaps()->originBottomLeft ? 1.0F : 0.0F};
    auto& items = transparent ? list.transparent : list.opaque;
    std::vector<render_detail::DrawBatch> temporary_batches;
    if ((transparent || capture) &&
        !render_detail::prepare_draw_batches(frame_data, list, draw, mode, temporary_batches))
    {
        return false;
    }
    const auto& batches = transparent || capture ? temporary_batches : list.batches;
    const bool use_ao = draw.ambient_occlusion && !depth_only && mode != SceneDrawMode::surface;
    const float ao[]{1.0F / draw.width, 1.0F / draw.height, use_ao ? 1.0F : 0.0F, 0};
    bgfx::setViewUniform(draw.id, camera_position, camera_position_value);
    bgfx::setViewUniform(draw.id, light_direction, zero);
    bgfx::setViewUniform(draw.id, light_color, zero);
    bgfx::setViewUniform(draw.id, ambient, ambient_value);
    bgfx::setViewUniform(draw.id, ambient_sky, sky);
    bgfx::setViewUniform(draw.id, ambient_equator, equator);
    bgfx::setViewUniform(draw.id, ambient_ground, ground);
    bgfx::setViewUniform(draw.id, fog_color, fog);
    bgfx::setViewUniform(draw.id, fog_range, fog_range_value);
    bgfx::setViewUniform(draw.id, tonemap, tone);
    bgfx::setViewUniform(draw.id, ibl_params, ibl);
    bgfx::setViewUniform(draw.id, capture_mode, capture_value);
    bgfx::setViewUniform(draw.id, frame_inputs, frame_value);
    bgfx::setViewUniform(draw.id, ao_settings, ao);
    bgfx::setViewUniform(draw.id, shadow_matrices, draw.shadow->matrices.data(),
                         render_detail::maximum_shadow_cascades);
    bgfx::setViewUniform(draw.id, shadow_rects, draw.shadow->rectangles.data(),
                         render_detail::maximum_shadow_cascades);
    bgfx::setViewUniform(draw.id, shadow_splits, draw.shadow->splits.data());
    bgfx::setViewUniform(draw.id, shadow_params, draw.shadow->parameters.data());
    bgfx::setViewUniform(draw.id, shadow_settings, shadow_values);
    const bool virtual_shadow_view =
        mode == SceneDrawMode::opaque && !draw.reflection && bgfx::isValid(draw.vsm.atlas);
    draw.cluster->bind_view(draw.id, virtual_shadow_view);
    [[maybe_unused]] std::uint32_t coverage_draws = 0;
    [[maybe_unused]] std::uint32_t solid_depth_draws = 0;
    [[maybe_unused]] std::uint32_t position_stream_draws = 0;
    [[maybe_unused]] std::uint32_t surface_material_draws = 0;
    [[maybe_unused]] std::uint32_t vsm_depth_range_draws = 0;
    [[maybe_unused]] std::uint32_t pbr_opaque_draws = 0;
    [[maybe_unused]] std::int64_t avoided_uniform_bytes = 0;
    for (const auto& batch : batches)
    {
        if (batch.meshlets.merged_into_previous)
        {
            continue;
        }
        std::size_t cursor = 0;
        while (cursor < batch.items.size())
        {
            const auto& item = items[batch.items[cursor]];
            const auto& mesh = frame_data.meshes[item.mesh];
            const auto& material = mesh.gpu->materials[item.slot];
            const auto* program = material.program;
            const bool coverage_depth =
                depth_only && !material.frame_textures && bgfx::isValid(program->depth_program);
            const bool solid_depth = coverage_depth && material.solid_depth;
            // Only built-in programs provide the coverage variant. Its paired
            // surface program consumes material and RVT data, without forward lighting.
            const bool material_surface =
                mode == SceneDrawMode::surface && bgfx::isValid(program->depth_program);
            const bool forward_inputs = !coverage_depth && !material_surface;
            const bool pbr_opaque = mode == SceneDrawMode::opaque && material.pbr_opaque;
            auto regular_program = pbr_opaque ? program->pbr_opaque_program : program->program;
            auto instanced_program =
                pbr_opaque ? program->instanced_pbr_opaque_program : program->instanced_program;
            if (mode == SceneDrawMode::surface)
            {
                if (!bgfx::isValid(program->surface_program))
                {
                    break;
                }
                regular_program = program->surface_program;
                instanced_program = program->instanced_surface_program;
                surface_material_draws += material_surface;
            }
            else if (coverage_depth)
            {
                regular_program =
                    solid_depth ? program->solid_depth_program : program->depth_program;
                instanced_program = solid_depth ? program->instanced_solid_depth_program
                                                : program->instanced_depth_program;
                ++coverage_draws;
                solid_depth_draws += solid_depth;
                position_stream_draws +=
                    solid_depth && !bgfx::isValid(mesh.gpu->deformed_vertices) &&
                    item.geometry->position_stride_bytes() ==
                        sizeof(render_detail::GpuVertex::position);
            }
            auto frame_textures = draw.textures;
            if (forward_inputs && !pbr_opaque)
            {
                std::array<float, 4> valid{};
                for (std::size_t index = 0; index < frame_textures.size(); ++index)
                {
                    valid[index] = bgfx::isValid(frame_textures[index]) ? 1.0F : 0.0F;
                    if (!bgfx::isValid(frame_textures[index]))
                    {
                        frame_textures[index] = ibl_lut_fallback;
                    }
                }
                std::array<float, 16> planar_matrix{};
                bx::mtxIdentity(planar_matrix.data());
                if (!draw.reflection && !capture)
                {
                    for (const auto& plane : targets().planar)
                    {
                        if (plane.node_id == mesh.gpu->node_id && bgfx::isValid(plane.texture()))
                        {
                            frame_textures[2] = plane.texture();
                            valid[2] = 1;
                            planar_matrix = plane.view_projection;
                            break;
                        }
                    }
                }
                bgfx::setUniform(frame_valid, valid.data());
                bgfx::setUniform(planar_projection, planar_matrix.data());
            }
            if (pbr_opaque)
            {
                // The built-in opaque variant declares no scene color, depth or planar inputs.
                avoided_uniform_bytes += 20 * sizeof(float);
            }
            if (!solid_depth &&
                !program->bind_material(material, frame_textures, draw.max_anisotropy, pbr_opaque))
            {
                return false;
            }
            if (!coverage_depth)
            {
                if (draw.rvt && bgfx::isValid(program->rvt_program) && !material.frame_textures)
                {
                    draw.rvt->bind(mesh.rvt_role == schema::RvtRole::reader ||
                                       mesh.rvt_role == schema::RvtRole::writer_reader,
                                   draw.world_origin);
                }
                else
                {
                    render_detail::RvtPass::bind_disabled();
                }
                const float surface_value[]{material.cull != schema::CullMode::back ? 1.0F : 0.0F,
                                            static_cast<float>(material.blend), 0, 0};
                bgfx::setUniform(surface, surface_value);
            }
            if (!solid_depth)
            {
                bgfx::setUniform(normal_transform, mesh.normal.data());
            }
            if (forward_inputs)
            {
                bgfx::setTexture(render_detail::ambient_occlusion_stage, ao_sampler,
                                 use_ao ? targets().ambient_occlusion.texture() : shadow_fallback);
                bgfx::setTexture(render_detail::ibl_irradiance_stage, ibl_irradiance_sampler,
                                 ibl_ready ? targets().ibl_textures[0]->handle : ibl_cube_fallback);
                bgfx::setTexture(render_detail::ibl_prefilter_stage, ibl_prefilter_sampler,
                                 ibl_ready ? targets().ibl_textures[1]->handle : ibl_cube_fallback);
                bgfx::setTexture(render_detail::ibl_lut_stage, ibl_lut_sampler,
                                 ibl_ready ? targets().ibl_textures[2]->handle : ibl_lut_fallback);
                const auto shadow_texture = targets().shadow.texture();
                bgfx::setTexture(render_detail::shadow_sampler_stage, shadow_sampler,
                                 bgfx::isValid(shadow_texture) ? shadow_texture : shadow_fallback);
                const bool virtual_shadow = virtual_shadow_view && !material.frame_textures;
                const bool depth_ranges = virtual_shadow && !item.has_lightmap &&
                                          bgfx::isValid(program->depth_program) &&
                                          bgfx::isValid(draw.vsm.depth_ranges);
                vsm_depth_range_draws += depth_ranges;
                const float lighting[]{item.has_lightmap ? 1.0F : 0.0F,
                                       item.has_probe ? 1.0F : 0.0F, depth_ranges ? 1.0F : 0.0F,
                                       depth_ranges ? draw.vsm.local_range_layout : 0.0F};
                bgfx::setUniform(lighting_params, lighting);
                if (!pbr_opaque || (!item.has_lightmap && item.has_probe))
                {
                    bgfx::setUniform(probe_sh, item.coefficients.data(), 9);
                }
                else
                {
                    avoided_uniform_bytes += sizeof(item.coefficients);
                }
                bgfx::setTexture(6, lightmap_sampler,
                                 depth_ranges ? draw.vsm.depth_ranges : item.lightmap,
                                 draw.max_anisotropy && !depth_ranges
                                     ? BGFX_SAMPLER_UVW_CLAMP | BGFX_SAMPLER_MIN_ANISOTROPIC |
                                           BGFX_SAMPLER_MAG_ANISOTROPIC
                                     : UINT32_MAX);
                draw.cluster->bind(virtual_shadow);
                if (virtual_shadow)
                {
                    bgfx::setTexture(10, draw.vsm.sampler, draw.vsm.atlas);
                }
            }
            bgfx::InstanceDataBuffer instance_buffer{};
            const bool indirect = bgfx::isValid(batch.meshlets.commands);
            const auto remaining = static_cast<std::uint32_t>(batch.items.size() - cursor);
            const auto available =
                !indirect && remaining > 1 && bgfx::isValid(instanced_program)
                    ? bgfx::getAvailInstanceDataBuffer(remaining, render_detail::instance_stride)
                    : 0;
            const auto count = available >= 2 ? available : 1U;
            if (count > 1)
            {
                ENGINE_PROFILE_ZONE("Render.instanceData");
                bgfx::allocInstanceDataBuffer(&instance_buffer, count,
                                              render_detail::instance_stride);
                for (std::uint32_t index = 0; index < count; ++index)
                {
                    const auto& instance =
                        frame_data.meshes[items[batch.items[cursor + index]].mesh];
                    auto* destination =
                        instance_buffer.data + index * render_detail::instance_stride;
                    std::memcpy(destination, instance.model.data(), 16 * sizeof(float));
                    std::memcpy(destination + 16 * sizeof(float), instance.normal.data(),
                                12 * sizeof(float));
                }
                bgfx::setInstanceDataBuffer(&instance_buffer);
            }
            if (indirect && remaining > 1)
            {
                bgfx::setInstanceDataBuffer(batch.meshlets.instances, 0,
                                            batch.meshlets.first_instance +
                                                batch.meshlets.instance_capacity);
            }
            const auto submitted_count = indirect ? remaining : count;
            if (depth_only || surface_writes_depth ||
                (mode == SceneDrawMode::opaque && draw.id == render_detail::main_view))
            {
                for (std::uint32_t index = 0; index < submitted_count; ++index)
                {
                    items[batch.items[cursor + index]].instanced_transform = submitted_count > 1;
                }
            }
            cursor += submitted_count;
            bgfx::setTransform(mesh.model.data());
            if (batch.meshlets.whole_vertex_page)
            {
                item.geometry->bind_vertices(solid_depth, true);
            }
            else if (item.geometry == mesh.gpu->geometry.get())
            {
                mesh.gpu->bind_vertices(solid_depth);
            }
            else
            {
                item.geometry->bind_vertices(solid_depth);
            }
            if (indirect)
            {
                if (bgfx::isValid(batch.meshlets.compacted_indices))
                {
                    bgfx::setIndexBuffer(batch.meshlets.compacted_indices);
                }
                else
                {
                    bgfx::setIndexBuffer(item.geometry->meshlet_indices(item.lod));
                }
            }
            else
            {
                bgfx::setIndexBuffer(item.geometry->index_buffer(item.lod), item.first, item.count);
            }
            auto state = batch.key.state & ~BGFX_STATE_MSAA;
            if (depth_only)
            {
                state &= ~(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
                if (draw.opaque_depth_prepared)
                {
                    bgfx::setStencil(BGFX_STENCIL_TEST_ALWAYS | BGFX_STENCIL_FUNC_REF(1) |
                                     BGFX_STENCIL_FUNC_RMASK(0xff) | BGFX_STENCIL_OP_FAIL_S_KEEP |
                                     BGFX_STENCIL_OP_FAIL_Z_KEEP | BGFX_STENCIL_OP_PASS_Z_REPLACE);
                }
            }
            else if (mode == SceneDrawMode::surface)
            {
                state &= ~(BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_MASK);
                // Equal-depth surfaces keep the original last contribution in submission order.
                state |= surface_writes_depth ? BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LEQUAL
                                              : BGFX_STATE_DEPTH_TEST_EQUAL;
            }
            else if (mode == SceneDrawMode::opaque && draw.opaque_depth_prepared)
            {
                // Consume each sample once so coplanar draws keep the original first winner.
                state = (state & ~(BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_MASK)) |
                        BGFX_STATE_DEPTH_TEST_EQUAL;
                bgfx::setStencil(BGFX_STENCIL_TEST_EQUAL | BGFX_STENCIL_FUNC_REF(1) |
                                 BGFX_STENCIL_FUNC_RMASK(0xff) | BGFX_STENCIL_OP_FAIL_S_KEEP |
                                 BGFX_STENCIL_OP_FAIL_Z_KEEP | BGFX_STENCIL_OP_PASS_Z_ZERO);
            }
            if (draw.multisampling)
            {
                state |= BGFX_STATE_MSAA;
            }
            bgfx::setState(state);
            ++stats.draw_calls;
            pbr_opaque_draws += pbr_opaque;
            if (indirect)
            {
                ++stats.indirect_batches;
                stats.indirect_commands += batch.meshlets.command_count;
                // Preserve the ordinary MVP arithmetic for singleton batches. Only
                // actual multi-instance batches consume the compacted transforms.
                const auto selected = remaining > 1 ? instanced_program : regular_program;
                bgfx::submit(draw.id, selected, batch.meshlets.commands,
                             batch.meshlets.first_command, batch.meshlets.command_count);
                continue;
            }
            stats.direct_instances += count;
            stats.direct_triangles += static_cast<std::uint64_t>(item.count / 3) * count;
            stats.direct_instance_batches += count > 1;
            bgfx::submit(draw.id, count > 1 ? instanced_program : regular_program);
        }
    }
    if (mode == SceneDrawMode::opaque && draw.id == render_detail::main_view)
    {
        ENGINE_PROFILE_PLOT("Vsm.depthRangeDraws",
                            static_cast<std::int64_t>(vsm_depth_range_draws));
        ENGINE_PROFILE_PLOT("Render.pbrOpaqueDraws", static_cast<std::int64_t>(pbr_opaque_draws));
        ENGINE_PROFILE_PLOT("Render.opaqueUniformBytesAvoided", avoided_uniform_bytes);
    }
    if (mode == SceneDrawMode::surface)
    {
        ENGINE_PROFILE_PLOT("Render.surfaceMaterialOnlyDraws",
                            static_cast<std::int64_t>(surface_material_draws));
    }
    if (depth_only && draw.opaque_depth_prepared && draw.reflection)
    {
        ENGINE_PROFILE_PLOT("Render.planarDepthCoverageDraws",
                            static_cast<std::int64_t>(coverage_draws));
    }
    else if (draw.id == render_detail::opaque_depth_view)
    {
        ENGINE_PROFILE_PLOT("Render.opaqueDepthCoverageDraws",
                            static_cast<std::int64_t>(coverage_draws));
        ENGINE_PROFILE_PLOT("Render.opaqueDepthPositionDraws",
                            static_cast<std::int64_t>(solid_depth_draws));
        ENGINE_PROFILE_PLOT("Render.opaqueDepthPositionStreamDraws",
                            static_cast<std::int64_t>(position_stream_draws));
    }
    else if (draw.id == render_detail::ambient_occlusion_depth_view)
    {
        ENGINE_PROFILE_PLOT("Render.sceneDepthCoverageDraws",
                            static_cast<std::int64_t>(coverage_draws));
    }
    const auto previous =
        std::find_if(view_stats.begin(), view_stats.end(),
                     [&stats](const auto& row)
                     {
                         ENGINE_PROFILE_FUNCTION_PROBE();
                         return row.view_id == stats.view_id;
                     });
    if (previous == view_stats.end())
    {
        view_stats.push_back(stats);
    }
    else
    {
        previous->candidate_meshes += stats.candidate_meshes;
        previous->culled_meshes += stats.culled_meshes;
        previous->visible_meshes += stats.visible_meshes;
        previous->draw_calls += stats.draw_calls;
        previous->direct_instance_batches += stats.direct_instance_batches;
        previous->direct_instances += stats.direct_instances;
        previous->direct_triangles += stats.direct_triangles;
    }
    return true;
}
} // namespace engine
