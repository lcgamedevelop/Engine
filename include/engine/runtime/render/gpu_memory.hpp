#pragma once

#include "runtime/scene/scene_loading.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace engine
{
enum class GpuMemoryKind : std::uint8_t
{
    texture,
    mesh_geometry,
    static_vertex_page,
    frame_output,
    frame_readback,
    count
};

enum class GpuMemoryPhase : std::uint8_t
{
    live,
    waiting_readback,
    waiting_retirement,
    destroy_queued,
    destroy_submitted,
    backend_reclaimed
};

struct GpuMemoryPoint
{
    std::uint64_t timestamp_ns = 0;
    std::uint32_t frame = 0;
};

struct GpuMemoryEntry
{
    std::uint64_t id = 0;
    GpuMemoryKind kind = GpuMemoryKind::texture;
    GpuMemoryPhase phase = GpuMemoryPhase::live;
    std::string guid;
    SceneScopeId creation_scope;
    std::uint64_t creation_scene = 0;
    std::uint64_t logical_bytes = 0;
    std::uint64_t shared_backing_bytes = 0;
    std::uint64_t shared_backing_id = 0;
    GpuMemoryPoint created;
    GpuMemoryPoint release_requested;
    GpuMemoryPoint readback_ready;
    GpuMemoryPoint destroy_queued;
    GpuMemoryPoint destroy_submitted;
    GpuMemoryPoint backend_reclaimed;
    bool drained_on_device_close = false;
};

struct GpuMemoryTotals
{
    std::uint64_t live_count = 0;
    std::uint64_t live_logical_bytes = 0;
    std::uint64_t live_shared_backing_bytes = 0;
    std::uint64_t retiring_count = 0;
    std::uint64_t retiring_logical_bytes = 0;
    std::uint64_t retiring_shared_backing_bytes = 0;
    std::uint64_t peak_logical_bytes = 0;
    std::uint64_t peak_shared_backing_bytes = 0;
};

struct GpuMemoryKindUsage
{
    std::uint64_t count = 0;
    std::uint64_t logical_bytes = 0;
    std::uint64_t shared_backing_bytes = 0;
};

struct GpuMemorySnapshot
{
    static constexpr std::uint32_t maximum_items = 64;
    static constexpr std::uint32_t recent_capacity = 128;

    std::uint64_t session_started_ns = 0;
    std::uint64_t sequence = 0;
    std::uint64_t device_generation = 0;
    GpuMemoryPoint sampled;
    GpuMemoryTotals totals;
    // Counts the bytes of accepted VSM feedback reads over this render device's lifetime.
    std::uint64_t vsm_feedback_readback_bytes_total = 0;
    std::array<GpuMemoryKindUsage, static_cast<std::size_t>(GpuMemoryKind::count)> live_by_kind{};
    std::optional<std::uint64_t> bgfx_texture_estimated_bytes;
    std::optional<std::uint64_t> bgfx_render_target_estimated_bytes;
    std::optional<std::uint64_t> driver_bytes;
    std::uint64_t texture_staging_reserved_bytes = 0;
    std::uint64_t retired_texture_staging_reserved_bytes = 0;
    std::uint64_t overwritten_history = 0;
    std::uint64_t matching_live = 0;
    std::uint64_t matching_pending = 0;
    std::uint64_t matching_reclaims = 0;
    std::vector<GpuMemoryEntry> live;
    std::vector<GpuMemoryEntry> pending;
    std::vector<GpuMemoryEntry> recent_reclaims;
};
} // namespace engine
