#include "runtime/performance/heap_profiling.hpp"

#include "runtime/performance/memory_trace.hpp"
#include "runtime/performance/profiling.hpp"

#include <algorithm>
#include <cerrno>
#include <utility>

#if defined(ENGINE_ENABLE_MACOS_HEAP_OBSERVATION)
#include <limits>
#include <pthread.h>
#endif

namespace engine::memory_profile::heap
{
namespace
{
#if defined(ENGINE_ENABLE_MACOS_HEAP_OBSERVATION)
constexpr auto invalid_key = std::numeric_limits<pthread_key_t>::max();
constinit std::atomic<pthread_key_t> pause_key{invalid_key};
char pause_value;

bool is_paused() noexcept
{
    const auto key = pause_key.load(std::memory_order_acquire);
    return key != invalid_key && pthread_getspecific(key) != nullptr;
}

bool set_paused(bool paused) noexcept
{
    const auto key = pause_key.load(std::memory_order_acquire);
    return key != invalid_key && pthread_setspecific(key, paused ? &pause_value : nullptr) == 0;
}
#else
thread_local bool paused = false;

bool is_paused() noexcept
{
    return paused;
}
#endif

void add_bytes(PoolUsage& pool, std::size_t bytes) noexcept
{
    pool.current_bytes += bytes;
    pool.peak_bytes = std::max(pool.peak_bytes, pool.current_bytes);
    ++pool.live_allocations;
}

void remove_bytes(PoolUsage& pool, std::size_t bytes) noexcept
{
    pool.current_bytes -= bytes;
    --pool.live_allocations;
}

bool largest_first(const HeapRecord& left, const HeapRecord& right) noexcept
{
    return left.requested_bytes != right.requested_bytes
               ? left.requested_bytes > right.requested_bytes
               : left.id > right.id;
}
} // namespace

constinit HeapAllocations allocations;

#if defined(ENGINE_ENABLE_MACOS_HEAP_OBSERVATION)
Pause::Pause() noexcept : previous_(is_paused())
{
    (void)set_paused(true);
}

Pause::~Pause() noexcept
{
    (void)set_paused(previous_);
}
#else
Pause::Pause() noexcept : previous_(std::exchange(paused, true))
{
}

Pause::~Pause() noexcept
{
    paused = previous_;
}
#endif

void HeapAllocations::start() noexcept
{
    ENGINE_PROFILE_FUNCTION();
    active_.store(true, std::memory_order_release);
}

bool HeapAllocations::observing() const noexcept
{
    return active_.load(std::memory_order_acquire) && !is_paused();
}

#if defined(ENGINE_ENABLE_MACOS_HEAP_OBSERVATION)
bool initialize_thread_guard() noexcept
{
    ENGINE_PROFILE_FUNCTION();
    // dyld can allocate while instantiating C++ thread_local storage.
    pthread_key_t key;
    if (pthread_key_create(&key, nullptr) != 0)
    {
        return false;
    }
    pause_key.store(key, std::memory_order_release);
    return true;
}

bool enter() noexcept
{
    return allocations.observing() && set_paused(true);
}

void leave() noexcept
{
    (void)set_paused(false);
}

void HeapAllocations::record_trace(Entry& entry) noexcept
{
#if defined(ENGINE_ENABLE_MEMORY_PROFILING_DETAILS)
    entry.connection = trace::allocate(trace::general_heap_pool,
                                      reinterpret_cast<const void*>(entry.address),
                                      entry.requested_bytes);
    if (entry.connection == 0)
    {
        ++totals_.capture_gaps;
    }
#else
    static_cast<void>(entry);
#endif
}

void HeapAllocations::release_trace(Entry& entry) noexcept
{
#if defined(ENGINE_ENABLE_MEMORY_PROFILING_DETAILS)
    if (entry.connection &&
        !trace::release(trace::general_heap_pool, reinterpret_cast<const void*>(entry.address),
                        entry.connection))
    {
        ++totals_.capture_gaps;
    }
    entry.connection = 0;
#else
    static_cast<void>(entry);
#endif
}
#endif

std::size_t HeapAllocations::bucket_for(std::uintptr_t address) noexcept
{
    auto value = address >> 3;
    value ^= value >> 17;
    value *= 0xed5ad4bbU;
    value ^= value >> 11;
    return value & (bucket_count - 1);
}

std::size_t HeapAllocations::find(std::uintptr_t address) const noexcept
{
    auto position = bucket_for(address);
    while (entries_[position].address && entries_[position].address != address)
    {
        position = (position + 1) & (bucket_count - 1);
    }
    return position;
}

void HeapAllocations::erase(std::size_t position) noexcept
{
    const auto& removed = entries_[position];
    remove_bytes(removed.attributions ? totals_.attributed : totals_.unattributed,
                 removed.requested_bytes);
    // Shift the suffix back where a removed entry would otherwise break a probe chain.
    constexpr auto mask = bucket_count - 1;
    for (auto next = (position + 1) & mask; entries_[next].address; next = (next + 1) & mask)
    {
        const auto origin = bucket_for(entries_[next].address);
        if (((position - origin) & mask) < ((next - origin) & mask))
        {
            entries_[position] = entries_[next];
            position = next;
        }
    }
    entries_[position] = {};
}

void HeapAllocations::allocated_locked(std::uintptr_t address, std::size_t bytes,
                                      const void* zone) noexcept
{
    auto position = find(address);
    if (entries_[position].address)
    {
        // An allocator boundary freed this address without reaching the observer.
        ++totals_.ownership_conflicts;
#if defined(ENGINE_ENABLE_MACOS_HEAP_OBSERVATION)
        release_trace(entries_[position]);
#endif
        erase(position);
        position = find(address);
    }
    const auto sequence = ++totals_.sequence;
    if (totals_.unattributed.live_allocations + totals_.attributed.live_allocations >=
        record_capacity)
    {
        ++totals_.dropped_allocations;
        return;
    }
    entries_[position] = {address, bytes, sequence, 0};
#if defined(ENGINE_ENABLE_MACOS_HEAP_OBSERVATION)
    entries_[position].zone = zone;
    record_trace(entries_[position]);
#else
    static_cast<void>(zone);
#endif
    add_bytes(totals_.unattributed, bytes);
    ++totals_.tracked_allocations;
}

void HeapAllocations::freed_locked(std::uintptr_t address) noexcept
{
    const auto position = find(address);
    auto& entry = entries_[position];
    const auto sequence = ++totals_.sequence;
    if (!entry.address)
    {
        ++totals_.unmatched_releases;
        return;
    }
    if (entry.attributions)
    {
        ++totals_.ownership_conflicts;
    }
#if defined(ENGINE_ENABLE_MACOS_HEAP_OBSERVATION)
    release_trace(entry);
#endif
    totals_.recent_releases[next_release_] = {entry.allocation_id, entry.requested_bytes, sequence};
    next_release_ = (next_release_ + 1) % HeapSnapshot::sample_limit;
    ++totals_.tracked_releases;
    erase(position);
}

void HeapAllocations::allocated(const void* address, std::size_t bytes, const void* zone) noexcept
{
    const std::lock_guard lock(mutex_);
    allocated_locked(reinterpret_cast<std::uintptr_t>(address), bytes, zone);
}

void HeapAllocations::freed(const void* address) noexcept
{
    const std::lock_guard lock(mutex_);
    freed_locked(reinterpret_cast<std::uintptr_t>(address));
}

void* HeapAllocations::reallocate(void* address, std::size_t bytes, RawReallocate raw,
                                 const void* argument, bool releases_on_failure,
                                 const void* zone) noexcept
{
    if (!observing())
    {
        return raw(address, bytes, argument);
    }
    const Pause pause;
    const auto previous = reinterpret_cast<std::uintptr_t>(address);
    void* result = nullptr;
    int saved_errno = 0;
    {
        // Serialize changes to the old identity against reuse on another thread.
        const std::lock_guard lock(mutex_);
#if defined(ENGINE_ENABLE_MACOS_HEAP_OBSERVATION)
        if (previous && !zone)
        {
            zone = entries_[find(previous)].zone;
        }
#endif
        result = raw(address, bytes, argument);
        saved_errno = errno;
        if (previous && (result || releases_on_failure))
        {
            freed_locked(previous);
        }
        if (result)
        {
            allocated_locked(reinterpret_cast<std::uintptr_t>(result), bytes, zone);
        }
    }
    errno = saved_errno;
    return result;
}

std::uint64_t HeapAllocations::attribute(const void* address) noexcept
{
    if (!active_.load(std::memory_order_acquire))
    {
        return 0;
    }
    const Pause pause;
    const std::lock_guard lock(mutex_);
    auto& entry = entries_[find(reinterpret_cast<std::uintptr_t>(address))];
    ++totals_.sequence;
    if (!entry.address)
    {
        ++totals_.unmatched_attributions;
        return 0;
    }
    if (entry.attributions++ == 0)
    {
#if defined(ENGINE_ENABLE_MACOS_HEAP_OBSERVATION)
        release_trace(entry);
#endif
        remove_bytes(totals_.unattributed, entry.requested_bytes);
        add_bytes(totals_.attributed, entry.requested_bytes);
    }
    else
    {
        ++totals_.ownership_conflicts;
    }
    return entry.allocation_id;
}

void HeapAllocations::unattribute(const void* address, std::uint64_t id) noexcept
{
    if (id == 0)
    {
        return;
    }
    const Pause pause;
    const std::lock_guard lock(mutex_);
    auto& entry = entries_[find(reinterpret_cast<std::uintptr_t>(address))];
    ++totals_.sequence;
    if (entry.allocation_id != id || entry.attributions == 0)
    {
        ++totals_.ownership_conflicts;
        return;
    }
    if (--entry.attributions == 0)
    {
        remove_bytes(totals_.attributed, entry.requested_bytes);
        add_bytes(totals_.unattributed, entry.requested_bytes);
#if defined(ENGINE_ENABLE_MACOS_HEAP_OBSERVATION)
        record_trace(entry);
#endif
    }
}

#if defined(ENGINE_ENABLE_MACOS_HEAP_OBSERVATION)
void HeapAllocations::destroy_zone(const void* zone) noexcept
{
    const std::lock_guard lock(mutex_);
    for (auto& entry : entries_)
    {
        while (entry.address && entry.zone == zone)
        {
            // Erasing can shift a later entry into this bucket.
            freed_locked(entry.address);
        }
    }
}
#endif

HeapSnapshot HeapAllocations::snapshot(bool include_records) const noexcept
{
    if (!active_.load(std::memory_order_acquire))
    {
        return {};
    }
    const Pause pause;
    const std::lock_guard lock(mutex_);
    auto result = totals_;
    result.available = true;
    result.record_capacity = record_capacity;
    result.metadata_bytes = sizeof(HeapAllocations);
    if (!include_records)
    {
        return result;
    }
    for (const auto& entry : entries_)
    {
        if (!entry.address || entry.attributions)
        {
            continue;
        }
        const HeapRecord candidate{entry.allocation_id, entry.requested_bytes, 0};
        if (!largest_first(candidate, result.largest.back()))
        {
            continue;
        }
        const auto position = std::lower_bound(result.largest.begin(), result.largest.end(),
                                               candidate, largest_first);
        std::move_backward(position, result.largest.end() - 1, result.largest.end());
        *position = candidate;
    }
    for (std::size_t offset = 0; offset < HeapSnapshot::sample_limit; ++offset)
    {
        const auto index = (next_release_ + HeapSnapshot::sample_limit - 1 - offset) %
                           HeapSnapshot::sample_limit;
        result.recent_releases[offset] = totals_.recent_releases[index];
    }
    return result;
}
} // namespace engine::memory_profile::heap
