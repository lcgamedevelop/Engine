#pragma once
#include "runtime/scene/scene_types.hpp"
#include <bit>
#include <cmath>
#include <cstdint>
#include <string>

namespace engine
{
struct RvtStatus
{
    bool requested = false, supported = false, effective = false;
    std::uint64_t device_generation = 0, demand_revision = 0, cache_revision = 0;
    std::uint32_t ready_pages = 0, required_pages = 0;
    std::uint64_t resident_bytes = 0;
    std::string reason;
};
enum class RvtDebugMode : std::uint8_t
{
    overview,
    virtual_texture,
    physical_atlas,
    page_table
};
enum class RvtDebugChannel : std::uint8_t
{
    base_color,
    normal,
    roughness,
    metallic,
    ao
};
struct RvtDebugOptions
{
    RvtDebugMode mode = RvtDebugMode::overview;
    RvtDebugChannel channel = RvtDebugChannel::base_color;
    std::int32_t mip = -1;
    bool operator==(const RvtDebugOptions&) const = default;
};
[[nodiscard]] inline bool valid_rvt_settings(const schema::RvtSettingsT& s) noexcept
{
    const auto& a = s.bounds.minimum();
    const auto& b = s.bounds.maximum();
    return std::isfinite(a.x()) && std::isfinite(a.y()) && std::isfinite(a.z()) &&
           std::isfinite(b.x()) && std::isfinite(b.y()) && std::isfinite(b.z()) && a.x() < b.x() &&
           a.y() < b.y() && a.z() < b.z() && std::has_single_bit(s.virtual_resolution) &&
           s.virtual_resolution >= 256 && s.virtual_resolution <= 32768 &&
           std::has_single_bit(s.page_size) && s.page_size >= 32 && s.page_size <= 256 &&
           s.virtual_resolution >= s.page_size && s.virtual_resolution / s.page_size <= 256 &&
           s.page_border >= 2 && s.page_border <= 16 && s.physical_pages >= 16 &&
           s.physical_pages <= 1024 && s.pages_per_frame > 0 && s.pages_per_frame <= 16;
}
} // namespace engine
