#include "runtime/render/passes/frame_output.hpp"

#include "runtime/performance/profiling.hpp"
#include "runtime/render/render_system.hpp"
#include "runtime/render/rvt/rvt_debug_view.hpp"
#include "runtime/render/rvt/rvt_pass.hpp"
#include "runtime/render/scene_draw.hpp"
#include "runtime/scene/scene.hpp"
#include "runtime/world/world_system.hpp"
#include <algorithm>
#include <bx/math.h>
#include <cmath>

namespace engine
{
bool RenderSystem::rvt_status(RvtStatus& output) const
{
    output = rvt_status_;
    if (!renderer_initialized_)
    {
        output = {};
        output.reason = "Device unavailable";
        return false;
    }
    return true;
}
bool RenderSystem::set_rvt_debug_options(const RvtDebugOptions& options)
{
    if (options.mode > RvtDebugMode::page_table || options.channel > RvtDebugChannel::ao ||
        options.mip < -1 || options.mip > 8 ||
        (rvt_cache_ && options.mip > rvt_cache_->maximum_mip()))
    {
        return false;
    }
    if (options != rvt_options_)
    {
        rvt_options_ = options;
        ++rvt_debug_revision_;
    }
    return true;
}
bool RenderSystem::rvt_debug_options(RvtDebugOptions& output, std::uint64_t& revision) const
{
    output = rvt_options_;
    revision = rvt_debug_revision_;
    return true;
}
void RenderSystem::release_rvt() noexcept
{
    rvt_cache_.reset();
    rvt_pass_.reset();
    rvt_scene_id_ = 0;
    rvt_status_ = {};
    rvt_status_.reason = world_ && world_->scene() ? "Disabled" : "No Scene";
}
void RenderSystem::prepare_rvt(const RenderSettings& settings,
                               const render_detail::RenderFrameData& frame,
                               const render_detail::SceneDrawView& draw)
{
    ENGINE_PROFILE_ZONE("Render.prepareRvt");
    if (!settings.atmosphere.rvt || !settings.atmosphere.rvt->enabled)
    {
        return;
    }
    const auto scene_id = world_->scene()->id();
    if (scene_id != rvt_scene_id_)
    {
        release_rvt();
        rvt_scene_id_ = scene_id;
    }
    const auto& config = *settings.atmosphere.rvt;
    if (!rvt_cache_)
    {
        rvt_cache_ = std::make_unique<render_detail::VirtualTextureCache>();
    }
    if (!rvt_pass_)
    {
        rvt_pass_ = std::make_unique<render_detail::RvtPass>();
    }
    rvt_status_ = {};
    rvt_status_.requested = true;
    rvt_status_.device_generation = device_generation_;
    if (!rvt_cache_->configure(config) || !rvt_pass_->configure(config, device_generation_))
    {
        rvt_status_.reason = rvt_pass_->reason();
        return;
    }
    rvt_cache_->begin_frame(device_generation_, frame_number_);
    rvt_pass_->invalidate_sources(*rvt_cache_, frame);
    float view_projection[16], inverse[16];
    bx::mtxMul(view_projection, draw.view, draw.projection);
    bx::mtxInverse(inverse, view_projection);
    std::array<double, 3> low{INFINITY, INFINITY, INFINITY}, high{-INFINITY, -INFINITY, -INFINITY};
    for (unsigned corner = 0; corner < 8; ++corner)
    {
        const bx::Vec3 clip{corner & 1 ? 1.F : -1.F, corner & 2 ? 1.F : -1.F,
                            corner & 4 ? 1.F : (bgfx::getCaps()->homogeneousDepth ? -1.F : 0.F)};
        const auto p = bx::mulH(clip, inverse);
        const double values[]{p.x + draw.world_origin.x(), p.y + draw.world_origin.y(),
                              p.z + draw.world_origin.z()};
        for (unsigned i = 0; i < 3; ++i)
        {
            low[i] = std::min(low[i], values[i]);
            high[i] = std::max(high[i], values[i]);
        }
    }
    render_detail::RvtPageDemand demand;
    demand.bounds = {{low[0], low[1], low[2]}, {high[0], high[1], high[2]}};
    demand.view_identity = draw.lod_observer[0];
    while (!rvt_cache_->request(demand))
    {
        if (demand.mip >= rvt_cache_->maximum_mip())
        {
            rvt_status_.reason = "Invalid RVT view demand";
            return;
        }
        ++demand.mip;
    }
    const SceneWorkBudget budget{config_.prepare_items_per_step, config_.prepare_seconds_per_step,
                                 config_.upload_bytes_per_step};
    if (rvt_cache_->prepare(budget) == SceneWorkState::failed ||
        !rvt_pass_->prepare_pages(*rvt_cache_, frame))
    {
        rvt_status_.supported = true;
        rvt_status_.reason = rvt_pass_->reason();
        return;
    }
    (void)rvt_cache_->status(rvt_status_);
    rvt_status_.supported = true;
    rvt_status_.effective = true;
    rvt_status_.resident_bytes = rvt_pass_->resident_bytes();
    std::uint32_t diagnostics = 0;
    for (unsigned bit = 0; bit < 3; ++bit)
    {
        if (debug_requests_[std::size_t(RenderFeature::rvt_pages) + bit])
        {
            diagnostics |= 1U << bit;
        }
    }
    rvt_pass_->set_diagnostics(diagnostics);
}
void RenderSystem::draw_rvt_viewer()
{
    if (!debug_requests_[std::size_t(RenderFeature::rvt_viewer)])
    {
        rvt_viewer_.reset();
        return;
    }
    if (!fonts_ || !world_ || !world_->resource_loader() || !frame_output_)
    {
        return;
    }
    if (!rvt_viewer_)
    {
        rvt_viewer_ = std::make_unique<render_detail::RvtDebugView>(*fonts_);
    }
    auto& loader = *world_->resource_loader();
    if (!rvt_viewer_->prepare_font(loader))
    {
        return;
    }
    rvt_debug_visible_ =
        rvt_viewer_->draw(rvt_status_, rvt_status_.effective ? rvt_pass_.get() : nullptr,
                          rvt_options_, *frame_output_);
}
} // namespace engine
