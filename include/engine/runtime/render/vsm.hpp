#pragma once

#include "runtime/scene/scene_loading.hpp"

#include <cstdint>
#include <string>

namespace engine
{
enum class VsmState : std::uint8_t
{
    disabled,
    warming,
    ready,
    budget_limited,
    unsupported,
    failed
};
struct VsmStatus
{
    VsmState state = VsmState::disabled;
    SceneScopeId scope;
    std::uint64_t light_instance = 0, device_generation = 0, submission_id = 0;
    std::uint64_t generation = 0, demand_revision = 0, cache_revision = 0;
    std::uint64_t view_revision = 0, demand_view_revision = 0;
    std::uint32_t resident_pages = 0, demanded_pages = 0, ready_pages = 0;
    std::uint32_t dirty_pages = 0, redrawn_pages = 0, fallback_pages = 0;
    std::uint64_t resident_bytes = 0;
    std::string error_code, error;
};
enum class VsmDebugMode : std::uint8_t
{
    overview,
    virtual_depth,
    physical_atlas,
    page_table,
    residency,
    invalidated
};
struct VsmDebugOptions
{
    VsmDebugMode mode = VsmDebugMode::overview;
    std::int32_t clipmap_level = -1;
    bool operator==(const VsmDebugOptions&) const = default;
};
} // namespace engine
