#include "runtime/render/render_system.hpp"

#include "runtime/performance/profiling.hpp"

#include "runtime/render/scene_gpu_resources.hpp"
#include "runtime/render/scene_render_query.hpp"
#include "runtime/render/vsm/virtual_shadow_cache.hpp"
#include "runtime/render/vsm/virtual_shadow_pass.hpp"
#include "runtime/render/vsm/vsm_debug_view.hpp"
#include "runtime/scene/scene.hpp"
#include "runtime/world/world_system.hpp"

#include <algorithm>
#include <bx/math.h>
#include <cmath>

namespace engine
{
bool RenderSystem::vsm_status(VsmStatus& output) const
{
    output = vsm_status_;
    if (!renderer_initialized_)
    {
        output = {};
        output.error_code = "device_unavailable";
        return false;
    }
    return true;
}
bool RenderSystem::set_vsm_debug_options(const VsmDebugOptions& options)
{
    if (options.mode > VsmDebugMode::invalidated || options.clipmap_level < -1 ||
        options.clipmap_level >= static_cast<int>(render_detail::vsm_max_levels) ||
        (options.clipmap_level >= 0 && vsm_level_count_ &&
         options.clipmap_level >= static_cast<int>(vsm_level_count_)))
    {
        return false;
    }
    if (options != vsm_options_)
    {
        vsm_options_ = options;
        ++vsm_debug_revision_;
    }
    return true;
}
bool RenderSystem::vsm_debug_options(VsmDebugOptions& output, std::uint64_t& revision) const
{
    output = vsm_options_;
    revision = vsm_debug_revision_;
    return true;
}
void RenderSystem::release_vsm() noexcept
{
    if (vsm_cache_)
    {
        vsm_cache_->reset_scene();
    }
    if (vsm_pass_)
    {
        vsm_pass_->reset_scene();
    }
    if (vsm_scene_id_)
    {
        ++vsm_generation_;
    }
    vsm_scene_id_ = vsm_light_instance_ = 0;
    vsm_status_ = {};
    vsm_level_count_ = 0;
}
bool RenderSystem::prepare_vsm(const render_detail::RenderFrameData& frame,
                               render_detail::SceneDrawView& draw, const CameraSettings& camera,
                               render_detail::VsmPageBatch& output)
{
    ENGINE_PROFILE_ZONE("Render.prepareVsm");
    output = {};
    const render_detail::LocalShadowLight* light = nullptr;
    for (const auto& item : frame.lights)
    {
        if (item.settings.kind == schema::LightKind::directional && item.settings.cast_shadows)
        {
            if (light)
            {
                return false;
            }
            light = &item;
        }
    }
    if (!light || light->settings.shadow_method != schema::ShadowMethod::virtual_shadow_map)
    {
        release_vsm();
        return true;
    }
    const schema::VsmSettingsT defaults;
    const auto& settings =
        light->settings.virtual_shadow ? *light->settings.virtual_shadow : defaults;
    const auto fail = [&](VsmState state, const char* code, const char* error)
    {
        if (vsm_cache_)
        {
            vsm_cache_->cancel_batch();
        }
        vsm_status_.state = state;
        vsm_status_.error_code = code;
        vsm_status_.error = error;
        return false;
    };
    if (!render_detail::vsm_supported(settings, config_.vsm_physical_pages))
    {
        return fail(VsmState::unsupported, "vsm_unsupported",
                    "Virtual shadow maps are unsupported");
    }
    const auto scene = world_->scene();
    if (vsm_scene_id_ != scene->id() || vsm_light_instance_ != light->instance_id ||
        vsm_settings_.virtual_resolution != settings.virtual_resolution ||
        vsm_settings_.first_clipmap_extent != settings.first_clipmap_extent ||
        vsm_settings_.lod_bias != settings.lod_bias)
    {
        release_vsm();
        ++vsm_generation_;
        vsm_scene_id_ = scene->id();
        vsm_light_instance_ = light->instance_id;
        vsm_settings_ = settings;
    }
    if (!vsm_cache_)
    {
        vsm_cache_ = std::make_unique<render_detail::VirtualShadowCache>();
    }
    if (!vsm_pass_)
    {
        vsm_pass_ = std::make_unique<render_detail::VirtualShadowPass>();
    }
    if (!vsm_cache_->configure(settings, config_, vsm_generation_) ||
        !vsm_pass_->prepare(settings, config_, vsm_generation_))
    {
        return fail(VsmState::failed, "vsm_prepare_failed",
                    "Cannot prepare virtual shadow resources");
    }
    render_detail::VsmView view;
    view.scope = {frame.world_epoch, render_detail::target_scope(*scene)};
    view.generation = vsm_generation_;
    view.device_generation = device_generation_;
    view.light_instance = light->instance_id;
    view.observer = draw.lod_observer[0];
    view.origin = {draw.world_origin.x(), draw.world_origin.y(), draw.world_origin.z()};
    const auto direction =
        bx::normalize(bx::Vec3{-light->world[8], -light->world[9], -light->world[10]});
    const bx::Vec3 up = std::abs(direction.y) > .95F ? bx::Vec3{0, 0, 1} : bx::Vec3{0, 1, 0};
    float light_view[16];
    bx::mtxLookAt(light_view, bx::Vec3{0, 0, 0}, direction, up, bx::Handedness::Right);
    for (unsigned axis = 0; axis < 3; ++axis)
    {
        for (unsigned row = 0; row < 3; ++row)
        {
            view.axes[axis][row] = light_view[row * 4 + axis];
        }
    }
    float view_projection[16];
    bx::mtxMul(view_projection, draw.view, draw.projection);
    bx::mtxInverse(view.inverse_view_projection.data(), view_projection);
    view.forward = {-draw.world[8], -draw.world[9], -draw.world[10]};
    view.pixel_scale = 2.F / (draw.height * std::abs(draw.projection[5]));
    view.orthographic = camera.projection == schema::Projection::orthographic;
    view.near_clip = camera.near_clip;
    view.shadow_distance = std::min(light->settings.shadow_distance, camera.far_clip);
    view.width = draw.width;
    view.height = draw.height;
    view.homogeneous_depth = bgfx::getCaps()->homogeneousDepth;
    view.origin_bottom_left = bgfx::getCaps()->originBottomLeft;
    if (!vsm_cache_->begin_frame(view, frame))
    {
        return fail(VsmState::failed, "vsm_view_failed", "Cannot prepare virtual shadow view");
    }
    render_detail::VsmPageDemand demand;
    while (vsm_pass_->take_demand(demand))
    {
        (void)vsm_cache_->request(demand); // Stale readbacks are deliberately discarded.
    }
    if (!vsm_cache_->plan(config_.vsm_pages_per_frame, output))
    {
        return fail(VsmState::failed, "vsm_plan_failed", "Cannot plan virtual shadow pages");
    }
    vsm_level_count_ = output.view.level_count;
    output.debug_options = vsm_options_;
    output.diagnostics = debug_requests_[std::size_t(RenderFeature::vsm_viewer)];
    if (!vsm_pass_->draw_pages(frame, output, scene_resources_->targets().shadow) ||
        !vsm_pass_->binding(draw.vsm))
    {
        return fail(VsmState::failed, "vsm_draw_failed", "Cannot submit virtual shadow pages");
    }
    return true;
}
void RenderSystem::draw_vsm_viewer()
{
    if (!debug_requests_[std::size_t(RenderFeature::vsm_viewer)])
    {
        vsm_viewer_.reset();
        return;
    }
    if (!fonts_ || !world_ || !world_->resource_loader() || !frame_output_)
    {
        return;
    }
    if (!vsm_viewer_)
    {
        vsm_viewer_ = std::make_unique<render_detail::VsmDebugView>(*fonts_);
    }
    if (!vsm_viewer_->prepare_font(*world_->resource_loader()))
    {
        return;
    }
    const auto* lighting =
        scene_resources_ && vsm_scene_id_ ? &scene_resources_->targets().clusters[2] : nullptr;
    vsm_debug_visible_ =
        vsm_viewer_->draw(vsm_status_, vsm_pass_.get(), lighting, vsm_options_, *frame_output_);
}
} // namespace engine
