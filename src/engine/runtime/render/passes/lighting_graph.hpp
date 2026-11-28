#pragma once

#include "runtime/render/frame_graph_plan.hpp"

#include <array>
#include <optional>

namespace engine::render_detail
{

enum class LightStage : FramePassId
{
    shadow,
    cluster_buffers,
    cluster_cull,
    publish
};

struct LightingGraph
{
    FrameGraphPlan plan;
    std::optional<bool> has_vsm_table;

    [[nodiscard]] bool prepare(bool vsm, FramePlanError& error)
    {
        if (has_vsm_table == vsm && plan.begin_cached_frame())
        {
            return true;
        }
        enum : FrameResourceId
        {
            casters,
            selected_lights,
            camera,
            vsm_table,
            atlas,
            shadow_records,
            buffers,
            light_clusters,
            bindings
        };
        constexpr FrameResourceId imports[]{casters, selected_lights, camera, vsm_table};
        constexpr FrameResourceId shadow_inputs[]{casters, selected_lights, camera};
        constexpr FrameResourceId shadow_outputs[]{atlas, shadow_records};
        constexpr FrameResourceId buffer_inputs[]{camera, vsm_table};
        constexpr FrameResourceId buffer_outputs[]{buffers};
        constexpr FrameResourceId cluster_inputs[]{buffers, selected_lights, camera, shadow_records,
                                                   vsm_table};
        constexpr FrameResourceId cluster_outputs[]{light_clusters};
        constexpr FrameResourceId publish_inputs[]{atlas, light_clusters};
        constexpr FrameResourceId root[]{bindings};
        const FramePassDeclaration passes[]{
            {static_cast<FramePassId>(LightStage::shadow), shadow_inputs, shadow_outputs},
            {static_cast<FramePassId>(LightStage::cluster_buffers),
             std::span(buffer_inputs).first(vsm ? 2 : 1), buffer_outputs},
            {static_cast<FramePassId>(LightStage::cluster_cull),
             std::span(cluster_inputs).first(vsm ? 5 : 4), cluster_outputs},
            {static_cast<FramePassId>(LightStage::publish), publish_inputs, root}};
        if (!plan.compile(passes, std::span(imports).first(vsm ? 4 : 3), root, error))
        {
            has_vsm_table.reset();
            return false;
        }
        has_vsm_table = vsm;
        return true;
    }
};

enum class PlanarStage : FramePassId
{
    depth,
    sky,
    opaque
};

struct PlanarGraph
{
    FrameGraphPlan plan;
    std::optional<bool> depth_prepass;

    [[nodiscard]] bool prepare(bool prepass, FramePlanError& error)
    {
        if (depth_prepass == prepass && plan.begin_cached_frame())
        {
            return true;
        }
        enum : FrameResourceId
        {
            visible_geometry,
            atmosphere,
            lighting,
            depth,
            clear_or_loaded_depth,
            sky_color,
            reflected_scene
        };
        constexpr FrameResourceId imports[]{visible_geometry, atmosphere, lighting};
        constexpr FrameResourceId depth_input[]{visible_geometry};
        constexpr FrameResourceId depth_output[]{depth};
        constexpr FrameResourceId sky_inputs[]{atmosphere, depth};
        constexpr FrameResourceId sky_outputs[]{clear_or_loaded_depth, sky_color};
        constexpr FrameResourceId opaque_inputs[]{visible_geometry, lighting, clear_or_loaded_depth,
                                                  sky_color};
        constexpr FrameResourceId root[]{reflected_scene};
        const FramePassDeclaration passes[]{
            {static_cast<FramePassId>(PlanarStage::depth), depth_input, depth_output},
            {static_cast<FramePassId>(PlanarStage::sky),
             std::span(sky_inputs).first(prepass ? 2 : 1), sky_outputs},
            {static_cast<FramePassId>(PlanarStage::opaque), opaque_inputs, root}};
        if (!plan.compile(std::span(passes).subspan(prepass ? 0 : 1), imports, root, error))
        {
            depth_prepass.reset();
            return false;
        }
        depth_prepass = prepass;
        return true;
    }
};

} // namespace engine::render_detail
