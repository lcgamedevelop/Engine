#pragma once

#include "runtime/render/gpu_memory.hpp"

#include <array>
#include <map>
#include <string_view>
#include <utility>

namespace engine::render_detail
{
// Only the bgfx API thread mutates the ledger. Each ticket outlives its actual GPU handle.
class GpuMemoryTracker final
{
public:
    GpuMemoryTracker(std::uint64_t device_generation, bool threaded) noexcept;
    GpuMemoryTracker(const GpuMemoryTracker&) = delete;
    GpuMemoryTracker& operator=(const GpuMemoryTracker&) = delete;

    void frame_submitted(std::uint32_t frame) noexcept;
    void device_drained() noexcept;
    [[nodiscard]] bool snapshot(std::string_view guid, std::uint32_t limit,
                                GpuMemorySnapshot& output) const;

private:
    friend class GpuMemoryAllocation;
    [[nodiscard]] std::uint64_t track(GpuMemoryEntry entry);
    void release_requested(std::uint64_t id) noexcept;
    void retain(std::uint64_t id) noexcept;
    void wait_for_readback(std::uint64_t id) noexcept;
    void readback_ready(std::uint64_t id) noexcept;
    void queue_destroy(std::uint64_t id) noexcept;
    void reclaim(GpuMemoryPoint sampled, bool device_close) noexcept;
    void plot() const noexcept;

    using Entries = std::map<std::uint64_t, GpuMemoryEntry>;
    Entries live_;
    Entries queued_;
    Entries submitted_;
    std::array<GpuMemoryEntry, GpuMemorySnapshot::recent_capacity> recent_;
    GpuMemoryTotals totals_;
    const std::uint64_t started_ns_;
    const std::uint64_t device_generation_;
    const bool threaded_;
    mutable std::uint64_t sequence_ = 0;
    std::uint64_t next_id_ = 0;
    std::uint64_t overwritten_history_ = 0;
    std::size_t recent_next_ = 0;
    std::size_t recent_count_ = 0;
    std::uint32_t frame_ = 0;
};

// A move transfers the observation alongside the handle. reset() follows the destroy call.
class GpuMemoryAllocation final
{
public:
    GpuMemoryAllocation() = default;
    ~GpuMemoryAllocation() noexcept;
    GpuMemoryAllocation(GpuMemoryAllocation&& other) noexcept;
    GpuMemoryAllocation& operator=(GpuMemoryAllocation&& other) noexcept;
    GpuMemoryAllocation(const GpuMemoryAllocation&) = delete;
    GpuMemoryAllocation& operator=(const GpuMemoryAllocation&) = delete;

    void track(GpuMemoryTracker* tracker, GpuMemoryEntry entry);
    void release_requested() noexcept;
    void retain() noexcept;
    void wait_for_readback() noexcept;
    void readback_ready() noexcept;
    void reset() noexcept;
    [[nodiscard]] std::uint64_t id() const noexcept;

private:
#if defined(ENGINE_ENABLE_MEMORY_PROFILING)
    GpuMemoryTracker* tracker_ = nullptr;
    std::uint64_t id_ = 0;
#endif
};
} // namespace engine::render_detail
