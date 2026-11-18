#pragma once

#include "runtime/render/vsm.hpp"

#include <array>
#include <bgfx/bgfx.h>
#include <compare>
#include <vector>

namespace engine::render_detail
{
inline constexpr std::uint32_t vsm_page_size = 128, vsm_border = 4;
inline constexpr std::uint32_t vsm_tile_size = vsm_page_size + vsm_border * 2;
inline constexpr std::uint32_t vsm_max_levels = 16, vsm_header_vectors = 8 + vsm_max_levels;

struct VsmPageKey
{
    std::uint8_t level = 0;
    std::int64_t x = 0, y = 0;
    auto operator<=>(const VsmPageKey&) const = default;
};
struct VsmClipmap
{
    std::int64_t x = 0, y = 0;
    double page_world_size = 0;
    bool operator==(const VsmClipmap&) const = default;
};
struct VsmView
{
    SceneScopeId scope;
    std::uint64_t generation = 0, device_generation = 0, light_instance = 0;
    std::uint64_t observer = 0, revision = 0;
    std::array<double, 3> origin{};
    std::array<std::array<double, 3>, 3> axes{};
    std::array<VsmClipmap, vsm_max_levels> levels{};
    std::array<float, 16> inverse_view_projection{};
    std::array<float, 3> forward{};
    double depth_min = 0, depth_max = 0;
    float pixel_scale = 0, near_clip = 0, shadow_distance = 0, lod_bias = 0;
    std::uint32_t pages_per_axis = 0, level_count = 0, physical_pages = 0;
    std::uint16_t width = 0, height = 0;
    bool homogeneous_depth = false, origin_bottom_left = false, orthographic = false;
    bool operator==(const VsmView&) const = default;
};
struct VsmPageDemand
{
    VsmView view;
    std::uint64_t submission_id = 0, receiver_revision = 0;
    std::vector<VsmPageKey> pages;
};
struct VsmPageDraw
{
    VsmPageKey key;
    std::uint32_t slot = 0;
    std::uint64_t version = 0;
    bgfx::ViewId view_id = 0;
    std::uint16_t x = 0, y = 0, size = 0;
    std::array<float, 16> view{}, projection{};
};
struct VsmPageBatch
{
    std::uint64_t generation = 0, batch_id = 0;
    VsmView view;
    VsmDebugOptions debug_options;
    bool diagnostics = false;
    std::vector<VsmPageDraw> pages;
    std::vector<std::uint32_t> page_table;
    std::vector<std::array<float, 4>> gpu_data;
};
struct VsmPageTarget
{
    bgfx::FrameBufferHandle framebuffer = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle fallback = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle depth_uniform = BGFX_INVALID_HANDLE;
    const std::vector<std::uint64_t>* caster_ids = nullptr;
};
struct VsmGpuBinding
{
    bgfx::TextureHandle atlas = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle sampler = BGFX_INVALID_HANDLE;
    std::uint64_t resident_bytes = 0;
    std::uint32_t atlas_size = 0, columns = 0;
};

[[nodiscard]] std::uint32_t vsm_atlas_columns(std::uint32_t pages) noexcept;
[[nodiscard]] bool vsm_supported(const schema::VsmSettingsT& settings,
                                 std::uint32_t physical_pages) noexcept;
void vsm_pack_view(const VsmView& view, std::vector<std::array<float, 4>>& output);
} // namespace engine::render_detail
