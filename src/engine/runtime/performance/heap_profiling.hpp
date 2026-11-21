#pragma once

#include "runtime/performance/memory_snapshot.hpp"

#include <atomic>
#include <mutex>

namespace engine::memory_profile::heap
{
class Pause final
{
public:
    Pause() noexcept;
    ~Pause() noexcept;
    Pause(const Pause&) = delete;
    Pause& operator=(const Pause&) = delete;

private:
    bool previous_;
};

using RawReallocate = void* (*)(void* address, std::size_t bytes, const void* argument) noexcept;

// Owns bounded metadata for instrumented system allocation entry points.
class HeapAllocations final
{
public:
    void start() noexcept;
    [[nodiscard]] bool observing() const noexcept;
    void allocated(const void* address, std::size_t bytes, const void* zone = nullptr) noexcept;
    void freed(const void* address) noexcept;
    [[nodiscard]] void* reallocate(void* address, std::size_t bytes, RawReallocate raw,
                                   const void* argument, bool releases_on_failure = false,
                                   const void* zone = nullptr) noexcept;
    [[nodiscard]] std::uint64_t attribute(const void* address) noexcept;
    void unattribute(const void* address, std::uint64_t id) noexcept;
    [[nodiscard]] HeapSnapshot snapshot(bool include_records = true) const noexcept;
#if defined(ENGINE_ENABLE_MACOS_HEAP_OBSERVATION)
    void destroy_zone(const void* zone) noexcept;
#endif

private:
#if defined(ENGINE_ENABLE_MACOS_HEAP_OBSERVATION)
    static constexpr std::size_t bucket_count = 262144;
#else
    static constexpr std::size_t bucket_count = 65536;
#endif
    static constexpr std::uint32_t record_capacity = bucket_count * 3 / 4;
    struct Entry
    {
        std::uintptr_t address = 0;
        std::size_t requested_bytes = 0;
        std::uint64_t allocation_id = 0;
        std::uint32_t attributions = 0;
#if defined(ENGINE_ENABLE_MACOS_HEAP_OBSERVATION)
        const void* zone = nullptr;
        std::uint64_t connection = 0;
#endif
    };
    [[nodiscard]] static std::size_t bucket_for(std::uintptr_t address) noexcept;
    [[nodiscard]] std::size_t find(std::uintptr_t address) const noexcept;
    void erase(std::size_t position) noexcept;
    void allocated_locked(std::uintptr_t address, std::size_t bytes, const void* zone) noexcept;
    void freed_locked(std::uintptr_t address) noexcept;
#if defined(ENGINE_ENABLE_MACOS_HEAP_OBSERVATION)
    void record_trace(Entry& entry) noexcept;
    void release_trace(Entry& entry) noexcept;
#endif

    std::atomic<bool> active_{false};
    mutable std::mutex mutex_;
    std::array<Entry, bucket_count> entries_{};
    HeapSnapshot totals_{};
    std::size_t next_release_ = 0;
};

extern HeapAllocations allocations;
#if defined(ENGINE_ENABLE_MACOS_HEAP_OBSERVATION)
[[nodiscard]] bool initialize_thread_guard() noexcept;
[[nodiscard]] bool enter() noexcept;
void leave() noexcept;
void install_macos_observer() noexcept;
#endif
} // namespace engine::memory_profile::heap
