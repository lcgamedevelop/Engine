#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace engine::memory_profile
{

enum class Mode : std::uint8_t
{
    off,
    totals,
    detailed,
};

enum class Pool : std::uint8_t
{
    resource_read,
    resource_decode,
    texture_prepare,
    texture_upload,
    quickjs,
    frame_readback,
    screenshot_encode,
    bgfx_cpu,
    jolt,
    flecs,
    count,
};

inline constexpr std::array pool_keys{
    "resourceRead", "resourceDecode", "texturePrepare", "textureUpload", "quickJs",
    "frameReadback", "screenshotEncode", "bgfxCpu", "jolt", "flecs"};
static_assert(pool_keys.size() == static_cast<std::size_t>(Pool::count));

struct PoolUsage
{
    std::uint64_t current_bytes = 0;
    std::uint64_t peak_bytes = 0;
    std::uint64_t live_allocations = 0;
};

struct HeapRecord
{
    std::uint64_t id = 0;
    std::uint64_t requested_bytes = 0;
    std::uint64_t release_sequence = 0;
};

struct HeapSnapshot
{
    static constexpr std::size_t sample_limit = 16;
    bool available = false;
    std::uint64_t sequence = 0;
    PoolUsage unattributed;
    PoolUsage attributed;
    std::uint64_t tracked_allocations = 0;
    std::uint64_t tracked_releases = 0;
    std::uint64_t dropped_allocations = 0;
    std::uint64_t unmatched_releases = 0;
    std::uint64_t unmatched_attributions = 0;
    std::uint64_t ownership_conflicts = 0;
    std::uint64_t capture_gaps = 0;
    std::uint32_t record_capacity = 0;
    std::uint32_t metadata_bytes = 0;
    std::array<HeapRecord, sample_limit> largest;
    std::array<HeapRecord, sample_limit> recent_releases;
};

struct MemorySnapshot
{
    std::uint64_t sequence = 0;
    std::uint64_t timestamp_ns = 0;
    std::uint64_t connection_id = 0;
    std::uint64_t capture_gaps = 0;
    Mode mode = Mode::off;
    bool connected = false;
    bool callstacks_available = false;
    std::array<PoolUsage, static_cast<std::size_t>(Pool::count)> pools{};
    HeapSnapshot general_heap;
};

// Copies counters at one point in time without walking the resource or task graph.
[[nodiscard]] MemorySnapshot snapshot() noexcept;

} // namespace engine::memory_profile
