#pragma once

#include "runtime/render/frame_graph_plan.hpp"
#include "runtime/render/render_constants.hpp"

#include <algorithm>

namespace engine::render_detail::vsm_graph
{
inline constexpr FramePassId feedback_readback = 0;
inline constexpr FramePassId page_draws = 0;
inline constexpr std::size_t maximum_local_faces = 24;

inline bool compile_feedback(FrameGraphPlan& plan, FramePlanError& error)
{
    constexpr FrameResourceId receiver_depth[]{0}, cleared_bits[]{1}, receiver_bits[]{2};
    constexpr FrameResourceId demand_reads[]{receiver_depth[0], cleared_bits[0]};
    const FramePassDeclaration passes[]{{vsm_feedback_clear_view, {}, cleared_bits},
                                        {vsm_feedback_view, demand_reads, receiver_bits},
                                        {feedback_readback, receiver_bits, {}, {}, true}};
    return plan.compile(passes, receiver_depth, {}, error);
}

// The shared shadow encoder prepares caster geometry once and has a writer for each dirty tile.
// Export just the atlas regions which it touched to the layered depth-range dispatch.
inline bool compile_pages(FrameGraphPlan& plan, std::size_t pages, bool ranges,
                          FramePlanError& error)
{
    if (pages > vsm_page_views.size())
    {
        error = {"Virtual shadow page count exceeds the reserved views"};
        return false;
    }
    constexpr FrameResourceId caster_streams[]{0};
    std::array<FrameResourceId, vsm_page_views.size()> depth{}, minmax{};
    for (std::size_t i = 0; i < pages; ++i)
    {
        depth[i] = static_cast<FrameResourceId>(1 + i);
        minmax[i] = static_cast<FrameResourceId>(1 + vsm_page_views.size() + i);
    }
    const auto tiles = std::span(depth).first(pages);
    const auto reduced = std::span(minmax).first(pages);
    const FramePassDeclaration passes[]{{page_draws, caster_streams, tiles},
                                        {vsm_depth_ranges_view, tiles, reduced}};
    const auto count = pages == 0 ? 0 : (ranges ? 2 : 1);
    return plan.compile(std::span(passes).first(count), caster_streams, ranges ? reduced : tiles,
                        error);
}

// Each dispatch reads up to sixteen ordinary atlas faces and writes their separate min/max rows.
inline bool compile_local_ranges(FrameGraphPlan& plan, std::size_t faces, FramePlanError& error)
{
    constexpr auto per_dispatch = vsm_page_views.size();
    if (faces > maximum_local_faces)
    {
        error = {"Local shadow range count exceeds the supported face limit"};
        return false;
    }
    std::array<FrameResourceId, maximum_local_faces> depth{}, minmax{};
    std::array<FramePassDeclaration, (maximum_local_faces + per_dispatch - 1) / per_dispatch>
        passes{};
    for (std::size_t i = 0; i < faces; ++i)
    {
        depth[i] = static_cast<FrameResourceId>(i);
        minmax[i] = static_cast<FrameResourceId>(maximum_local_faces + i);
    }
    std::size_t groups = 0;
    for (std::size_t first = 0; first < faces; first += per_dispatch)
    {
        const auto count = std::min(per_dispatch, faces - first);
        passes[groups] = {static_cast<FramePassId>(groups), std::span(depth).subspan(first, count),
                          std::span(minmax).subspan(first, count)};
        ++groups;
    }
    return plan.compile(std::span(passes).first(groups), std::span(depth).first(faces),
                        std::span(minmax).first(faces), error);
}
} // namespace engine::render_detail::vsm_graph
