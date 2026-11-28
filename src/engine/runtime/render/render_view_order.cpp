#include "runtime/render/render_view_order.hpp"

#include "runtime/render/render_constants.hpp"

#include <array>
#include <vector>

namespace engine::render_detail
{
namespace
{
using View = bgfx::ViewId;
constexpr View view_count = 256;
constexpr View shadow_faces = 28;
constexpr View rvt_clear = rvt_page_view - 1;
constexpr View rvt_end = rvt_page_view + maximum_rvt_views;
static_assert(view_count <= frame_pass_capacity && view_count <= frame_resource_capacity);

// Physical completion of a View covers all commands in that View. A read imports its producer's
// framebuffer/buffer outputs; an order edge fences reuse or a reserved external batch. Unused
// slots emit no GPU work, but remain available to bgfx clients in the complete permutation.
struct ViewUses
{
    std::vector<FrameResourceId> reads;
    std::vector<FramePassId> after;
};

using DeviceViews = std::array<ViewUses, view_count>;

void declare_observer(DeviceViews& views, View shadows, View sky_lut, View color)
{
    const auto cluster = static_cast<View>(shadows + shadow_faces);
    for (View face = shadows; face < cluster; ++face)
    {
        views[face].reads.push_back(skinning_view);
        views[color].reads.push_back(face);
    }
    views[color].reads.push_back(cluster);
    views[color].reads.push_back(sky_lut);
    views[sky_lut].reads = {atmosphere_transmittance_view, atmosphere_multiscatter_view};
}

void declare_geometry(DeviceViews& views)
{
    views[atmosphere_multiscatter_view].reads = {atmosphere_transmittance_view};
    for (View page = rvt_page_view; page < rvt_end; ++page)
    {
        views[page].reads = {rvt_clear, skinning_view};
        // Keep the complete reserved terrain update ahead of scene observers, including empty
        // shadow slots that can be reassigned between cascades and local-light faces.
        for (View slot = 3; slot <= surface_view; ++slot)
        {
            views[slot].after.push_back(page);
        }
    }
    for (const View depth : planar_depth_views)
    {
        const auto color = static_cast<View>(depth - 1);
        declare_observer(views, color - 30, color - 1, color);
        views[depth].reads.push_back(skinning_view);
        views[color].reads.push_back(depth);
        views[main_view].reads.push_back(color);
    }
    declare_observer(views, shadow_view, atmosphere_main_skyview, main_view);
    declare_observer(views, capture_shadow_view, atmosphere_capture_skyview, capture_view);
    views[meshlet_cull_view].reads = {meshlet_clear_view};
    views[meshlet_index_compact_view].reads = {meshlet_index_upload_view, meshlet_cull_view};
    for (const View raster : {ambient_occlusion_depth_view, surface_view, opaque_depth_view,
                              main_view, meshlet_color_view})
    {
        views[raster].reads.push_back(skinning_view);
        views[raster].reads.push_back(meshlet_index_compact_view);
    }
    views[main_view].reads.push_back(opaque_depth_view);
    views[main_view].reads.push_back(ambient_occlusion_compute_view);
    views[ambient_occlusion_compute_view].reads = {ambient_occlusion_depth_view, surface_view};
}

void declare_virtual_shadows(DeviceViews& views)
{
    views[vsm_feedback_clear_view].after = {surface_view};
    views[vsm_feedback_view].reads = {surface_view, vsm_feedback_clear_view};
    for (const auto page : vsm_page_views)
    {
        views[page].reads = {skinning_view};
        // The CPU plans resident tiles from earlier feedback. This frame's feedback uses its
        // reserved position independently of the actual selection.
        views[page].after = {vsm_feedback_view};
        views[vsm_depth_ranges_view].reads.push_back(page);
        views[main_view].reads.push_back(page);
    }
    for (View face = shadow_view; face < shadow_view + shadow_faces; ++face)
    {
        views[local_shadow_depth_ranges_view].reads.push_back(face);
    }
    views[local_shadow_depth_ranges_view].after = {vsm_depth_ranges_view};
    views[ambient_occlusion_compute_view].after = {local_shadow_depth_ranges_view};
    views[opaque_depth_view].after.push_back(ambient_occlusion_compute_view);
    views[main_view].reads.push_back(vsm_depth_ranges_view);
    views[main_view].reads.push_back(local_shadow_depth_ranges_view);
}

void declare_output(DeviceViews& views)
{
    // Scene color and the movie output are exclusive. The movie shares the first SSR mip slot;
    // use the union of their physical prerequisites, leaving inactive candidates idle.
    static_assert(movie_view == ssr_hiz_view);
    for (View slot = main_view + 1; slot < local_shadow_depth_ranges_view; ++slot)
    {
        views[slot].after.push_back(main_view);
    }
    views[ssr_hiz_view].reads = {surface_view};
    for (View mip = ssr_hiz_view + 1; mip < ssr_trace_view; ++mip)
    {
        views[mip].reads = {static_cast<View>(mip - 1)};
    }
    views[ssr_trace_view].reads = {surface_view, main_view};
    for (View mip = ssr_hiz_view; mip < ssr_trace_view; ++mip)
    {
        views[ssr_trace_view].reads.push_back(mip);
    }
    views[ssr_composite_view].reads = {main_view, ssr_trace_view};
    views[scene_copy_view].reads = {main_view, ssr_composite_view};
    views[transparent_view].reads = {scene_copy_view, meshlet_color_view};
    views[extension_view].reads = {transparent_view};
    views[debug_view].reads = {extension_view};
    views[hdr_bright_view].reads = {debug_view};
    views[hdr_blur_view].reads = {hdr_bright_view};
    views[hdr_resolve_view].reads = {debug_view, hdr_blur_view};
    views[hdr_capture_view].reads = {debug_view};
    views[hdr_capture_view].after = {hdr_resolve_view};

    views[ui_view].reads = {output_clear_view, hdr_resolve_view, movie_view};
    views[rvt_debug_view].reads = {ui_view};
    views[vsm_debug_view].reads = {rvt_debug_view, vsm_depth_ranges_view};
    views[output_present_view].reads = {vsm_debug_view};
    views[output_capture_view].reads = {vsm_debug_view};
    views[output_capture_view].after = {output_present_view};

    // External and capture jobs have slots following the window output. IBL compute/readback is
    // an editor reservation. The core does not depend on its optional producer.
    for (View slot = rvt_end; slot < view_count; ++slot)
    {
        views[slot].after.push_back(output_capture_view);
    }
    views[bake_copy_view].reads = {bake_compute_view};
    views[capture_copy_view].reads = {capture_view};
}
} // namespace

bool compile_device_views(FrameGraphPlan& plan, std::uint16_t available_views,
                          FramePlanError& error)
{
    if (available_views < view_count)
    {
        error = {"Device has fewer Views than the reserved GPU schedule"};
        return false;
    }
    DeviceViews views;
    declare_geometry(views);
    declare_virtual_shadows(views);
    declare_output(views);
    std::array<FrameResourceId, view_count> outputs{};
    std::array<FramePassDeclaration, view_count> declarations{};
    for (View id = 0; id < view_count; ++id)
    {
        outputs[id] = id;
        declarations[id] = {id, views[id].reads, std::span(outputs).subspan(id, 1),
                            views[id].after};
    }
    // Independent slots use numeric View ID as their stable tie-break. Every reserved slot is
    // exported so jobs outside the runtime can submit before bgfx::frame as well.
    return plan.compile(declarations, {}, outputs, error);
}

} // namespace engine::render_detail
