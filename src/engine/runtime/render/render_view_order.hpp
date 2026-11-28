#pragma once

#include "runtime/render/render_constants.hpp"

#include <algorithm>
#include <array>
#include <cstddef>

namespace engine::render_detail
{
// The complete scene batch includes slots owned by UI, presentation, readback and editor jobs.
inline constexpr auto scene_view_order = []
{
    // Explicit GPU dependencies: deformation -> RVT -> depth -> VSM -> opaque -> UI -> capture.
    std::array<bgfx::ViewId, 256> order{};
    std::array<bool, order.size()> used{};
    std::size_t next_view = 0;
    const auto append = [&](bgfx::ViewId id)
    {
        if (!used[id])
        {
            order[next_view++] = id;
            used[id] = true;
        }
    };
    for (bgfx::ViewId id = 0; id < 3; ++id)
    {
        append(id);
    }
    for (bgfx::ViewId id = rvt_page_view - 1; id < rvt_page_view + maximum_rvt_views; ++id)
    {
        append(id);
    }
    for (bgfx::ViewId id = 3; id <= surface_view; ++id)
    {
        const auto planar_depth = static_cast<bgfx::ViewId>(id + 1);
        if (std::ranges::find(planar_depth_views, planar_depth) != planar_depth_views.end())
        {
            append(planar_depth);
        }
        if (id != opaque_depth_view)
        {
            append(id);
        }
    }
    append(vsm_feedback_clear_view);
    append(vsm_feedback_view);
    for (auto id : vsm_page_views)
    {
        append(id);
    }
    append(vsm_depth_ranges_view);
    append(local_shadow_depth_ranges_view);
    for (bgfx::ViewId id = surface_view + 1; id < order.size(); ++id)
    {
        if (id == main_view)
        {
            append(opaque_depth_view);
        }
        if (id == output_present_view)
        {
            append(vsm_debug_view);
        }
        append(id);
    }
    return order;
}();
} // namespace engine::render_detail
