#pragma once

#include "runtime/performance/function_profile.hpp"

#include "runtime/render/render_features.hpp"

#include <array>
#include <string_view>

namespace engine::render_detail
{
inline constexpr std::array<std::string_view, static_cast<std::size_t>(RenderFeature::count)>
    feature_names{"frustum_culling",
                  "static_instancing",
                  "static_lod",
                  "meshlet_culling",
                  "wireframe",
                  "debug_draw",
                  "mesh_bounds",
                  "camera_frusta",
                  "meshlet_bounds",
                  "meshlet_colors",
                  "meshlet_statistics",
                  "rvt_viewer",
                  "rvt_pages",
                  "rvt_residency",
                  "rvt_invalidated",
                  "vsm_viewer",
                  "stats"};

inline bool parse_feature(std::string_view name, RenderFeature& output) noexcept
{
    ENGINE_PROFILE_FUNCTION_PROBE();
    for (std::size_t index = 0; index < feature_names.size(); ++index)
    {
        if (feature_names[index] == name)
        {
            output = static_cast<RenderFeature>(index);
            return true;
        }
    }
    return false;
}

inline std::string_view feature_reason_name(RenderFeatureReason reason) noexcept
{
    ENGINE_PROFILE_FUNCTION_PROBE();
    constexpr std::array names{"none",
                               "device_unavailable",
                               "device_unsupported",
                               "no_scene",
                               "no_applicable_draws",
                               "resources_unavailable",
                               "capacity_limited"};
    const auto index = static_cast<std::size_t>(reason);
    return index < names.size() ? names[index] : "device_unsupported";
}
} // namespace engine::render_detail
