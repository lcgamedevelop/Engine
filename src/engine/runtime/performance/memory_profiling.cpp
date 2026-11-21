#include "runtime/performance/memory_profiling.hpp"

#if defined(ENGINE_ENABLE_MEMORY_PROFILING)
#include "runtime/performance/memory_trace.hpp"
#include "runtime/performance/profiling.hpp"

#if defined(ENGINE_ENABLE_GENERAL_HEAP_OBSERVATION)
#include "runtime/performance/heap_profiling.hpp"
#endif

#include <algorithm>
#include <bit>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <utility>
#endif

namespace engine::memory_profile
{

#if defined(ENGINE_ENABLE_MEMORY_PROFILING)
namespace
{
using Clock = std::chrono::steady_clock;

#if defined(ENGINE_ENABLE_TRACY)
struct PoolNames
{
    const char* events;
    const char* bytes;
    const char* peak;
};

constexpr std::array<PoolNames, static_cast<std::size_t>(Pool::count)> names{{
    {"Engine.ResourceRead", "Memory.resourceReadBytes", "Memory.resourceReadPeakBytes"},
    {"Engine.ResourceDecode", "Memory.resourceDecodeBytes", "Memory.resourceDecodePeakBytes"},
    {"Engine.TexturePrepare", "Memory.texturePrepareBytes", "Memory.texturePreparePeakBytes"},
    {"Engine.TextureUpload", "Memory.textureUploadBytes", "Memory.textureUploadPeakBytes"},
    {"Engine.QuickJS", "Memory.quickJsBytes", "Memory.quickJsPeakBytes"},
    {"Engine.FrameReadback", "Memory.frameReadbackBytes", "Memory.frameReadbackPeakBytes"},
    {"Engine.ScreenshotEncode", "Memory.screenshotEncodeBytes", "Memory.screenshotEncodePeakBytes"},
    {"Engine.BgfxCpu", "Memory.bgfxCpuBytes", "Memory.bgfxCpuPeakBytes"},
    {"Engine.Jolt", "Memory.joltBytes", "Memory.joltPeakBytes"},
    {"Engine.Flecs", "Memory.flecsBytes", "Memory.flecsPeakBytes"},
}};
#endif

#if defined(ENGINE_ENABLE_MEMORY_PROFILING_DETAILS)
constexpr Mode mode = Mode::detailed;
constexpr int stack_depth = 16;
#else
constexpr Mode mode = Mode::totals;
#endif

std::mutex counters_mutex;
MemorySnapshot counters;

bool has_callstacks() noexcept
{
#if defined(ENGINE_ENABLE_MEMORY_PROFILING_DETAILS)
    return tracy::has_callstack();
#else
    return false;
#endif
}

std::uint64_t current_connection() noexcept
{
#if defined(ENGINE_ENABLE_TRACY)
    if (ENGINE_PROFILE_CONNECTED())
    {
#if defined(TRACY_ON_DEMAND)
        return tracy::GetProfiler().ConnectionId();
#else
        return 1;
#endif
    }
#endif
    return 0;
}

const char* trace_pool_name(Pool pool) noexcept
{
#if defined(ENGINE_ENABLE_TRACY)
    return names[static_cast<std::size_t>(pool)].events;
#else
    static_cast<void>(pool);
    return nullptr;
#endif
}
} // namespace

namespace trace
{
std::uint64_t allocate(const char* name, const void* address, std::size_t capacity) noexcept
{
#if defined(ENGINE_ENABLE_MEMORY_PROFILING_DETAILS)
    const auto connection = current_connection();
    if (connection == 0)
    {
        return 0;
    }
    if (has_callstacks())
    {
        TracyAllocNS(address, capacity, stack_depth, name);
    }
    else
    {
        TracyAllocN(address, capacity, name);
    }
    return current_connection() == connection ? connection : 0;
#else
    static_cast<void>(name);
    static_cast<void>(address);
    static_cast<void>(capacity);
    return 0;
#endif
}

bool release(const char* name, const void* address, std::uint64_t connection) noexcept
{
#if defined(ENGINE_ENABLE_MEMORY_PROFILING_DETAILS)
    if (connection == 0 || current_connection() != connection)
    {
        return false;
    }
    if (has_callstacks())
    {
        TracyFreeNS(address, stack_depth, name);
    }
    else
    {
        TracyFreeN(address, name);
    }
    return current_connection() == connection;
#else
    static_cast<void>(name);
    static_cast<void>(address);
    static_cast<void>(connection);
    return false;
#endif
}
} // namespace trace

namespace
{
// The caller holds counters_mutex. This orders events if realloc makes an old
// address available to another thread before the old lifetime can be reported.
std::uint64_t acquire(Pool pool, const void* address, std::size_t capacity) noexcept
{
    const auto connection = trace::allocate(trace_pool_name(pool), address, capacity);
    auto& usage = counters.pools[static_cast<std::size_t>(pool)];
    usage.current_bytes += capacity;
    usage.peak_bytes = std::max(usage.current_bytes, usage.peak_bytes);
    ++usage.live_allocations;
    ++counters.sequence;
    if (mode == Mode::detailed && connection == 0)
    {
        ++counters.capture_gaps;
    }
    return connection;
}

void release(Pool pool, const void* address, std::size_t capacity,
             std::uint64_t connection) noexcept
{
    const bool recorded = trace::release(trace_pool_name(pool), address, connection);
    auto& usage = counters.pools[static_cast<std::size_t>(pool)];
    usage.current_bytes -= capacity;
    --usage.live_allocations;
    ++counters.sequence;
    if (connection != 0 && !recorded)
    {
        ++counters.capture_gaps;
    }
}

} // namespace

Allocation::Allocation(Allocation&& other) noexcept
    : address_(std::exchange(other.address_, nullptr)), capacity_(other.capacity_),
      connection_id_(other.connection_id_), heap_allocation_id_(other.heap_allocation_id_),
      pool_(other.pool_)
{}

Allocation& Allocation::operator=(Allocation&& other) noexcept
{
    if (this != &other)
    {
        reset();
        address_ = std::exchange(other.address_, nullptr);
        capacity_ = other.capacity_;
        connection_id_ = other.connection_id_;
        heap_allocation_id_ = other.heap_allocation_id_;
        pool_ = other.pool_;
    }
    return *this;
}

Allocation::~Allocation()
{
    reset();
}

void Allocation::track(Pool pool, const void* address, std::size_t capacity) noexcept
{
    reset();
    if (!address || capacity == 0)
    {
        return;
    }
    address_ = address;
    capacity_ = capacity;
    pool_ = pool;
    const std::lock_guard lock(counters_mutex);
#if defined(ENGINE_ENABLE_GENERAL_HEAP_OBSERVATION)
    const heap::Pause pause;
    heap_allocation_id_ = heap::allocations.attribute(address);
#endif
    connection_id_ = acquire(pool, address, capacity);
}

void Allocation::reset() noexcept
{
    if (!address_)
    {
        return;
    }
    const std::lock_guard lock(counters_mutex);
#if defined(ENGINE_ENABLE_GENERAL_HEAP_OBSERVATION)
    const heap::Pause pause;
#endif
    release(pool_, address_, capacity_, connection_id_);
#if defined(ENGINE_ENABLE_GENERAL_HEAP_OBSERVATION)
    heap::allocations.unattribute(address_, heap_allocation_id_);
#endif
    address_ = nullptr;
}

void* BackingAllocation::allocate_block(Pool pool, std::size_t capacity, bool zeroed) noexcept
{
    return allocate(pool, capacity, 0, zeroed);
}

void* BackingAllocation::allocate_aligned_block(Pool pool, std::size_t capacity,
                                                std::size_t alignment) noexcept
{
    return allocate(pool, capacity, alignment, false);
}

void* BackingAllocation::allocate(Pool pool, std::size_t capacity, std::size_t alignment,
                                  bool zeroed) noexcept
{
    if (alignment != 0 && !std::has_single_bit(alignment))
    {
        return nullptr;
    }
    constexpr auto natural_alignment = alignof(BackingAllocation);
    const auto padding = alignment > natural_alignment ? alignment - 1 : 0;
    constexpr auto maximum = std::numeric_limits<std::size_t>::max();
    if (capacity == 0 || capacity > maximum - sizeof(BackingAllocation) - padding)
    {
        return nullptr;
    }
    const auto bytes = sizeof(BackingAllocation) + capacity + padding;
#if defined(ENGINE_ENABLE_GENERAL_HEAP_OBSERVATION)
    const heap::Pause pause;
#endif
    auto* base = static_cast<std::byte*>(zeroed ? std::calloc(1, bytes) : std::malloc(bytes));
    if (!base)
    {
        return nullptr;
    }
    void* address = base + sizeof(BackingAllocation);
    if (padding != 0)
    {
        auto space = capacity + padding;
        (void)std::align(alignment, capacity, address, space);
    }
    auto* block = header(address);
    block->capacity_ = capacity;
    block->offset_ = static_cast<std::size_t>(static_cast<std::byte*>(address) - base);
    const std::lock_guard lock(counters_mutex);
    block->connection_id_ = acquire(pool, address, capacity);
    return address;
}

BackingAllocation* BackingAllocation::header(void* address) noexcept
{
    return reinterpret_cast<BackingAllocation*>(static_cast<std::byte*>(address) -
                                                sizeof(BackingAllocation));
}

void* BackingAllocation::reallocate_block(Pool pool, void* address, std::size_t capacity,
                                          std::size_t alignment) noexcept
{
    if (!address)
    {
        return allocate_aligned_block(pool, capacity, alignment);
    }
    if (capacity == 0)
    {
        free_block(pool, address);
        return nullptr;
    }
    if ((alignment != 0 && !std::has_single_bit(alignment)) ||
        capacity > std::numeric_limits<std::size_t>::max() - sizeof(BackingAllocation))
    {
        return nullptr;
    }
    auto* block = header(address);
    const auto old_capacity = block->capacity_;
    const auto old_address = reinterpret_cast<std::uintptr_t>(address);
    if (old_capacity == capacity && (alignment == 0 || old_address % alignment == 0))
    {
        return address;
    }
    if (alignment > alignof(BackingAllocation) || block->offset_ != sizeof(BackingAllocation))
    {
        auto* resized = allocate_aligned_block(pool, capacity, alignment);
        if (resized)
        {
            std::memcpy(resized, address, std::min(old_capacity, capacity));
            free_block(pool, address);
        }
        return resized;
    }
    const auto old_connection = block->connection_id_;
    const std::lock_guard lock(counters_mutex);
#if defined(ENGINE_ENABLE_GENERAL_HEAP_OBSERVATION)
    const heap::Pause pause;
#endif
    auto* resized =
        static_cast<BackingAllocation*>(std::realloc(block, sizeof(BackingAllocation) + capacity));
    if (!resized)
    {
        return nullptr;
    }
    release(pool, reinterpret_cast<const void*>(old_address), old_capacity, old_connection);
    resized->capacity_ = capacity;
    resized->connection_id_ = acquire(pool, resized + 1, capacity);
    return resized + 1;
}

void BackingAllocation::free_block(Pool pool, void* address) noexcept
{
    if (!address)
    {
        return;
    }
#if defined(ENGINE_ENABLE_GENERAL_HEAP_OBSERVATION)
    const heap::Pause pause;
#endif
    const auto* block = header(address);
    {
        const std::lock_guard lock(counters_mutex);
        release(pool, address, block->capacity_, block->connection_id_);
    }
    std::free(static_cast<std::byte*>(address) - block->offset_);
}

std::size_t BackingAllocation::capacity(const void* address) noexcept
{
    if (!address)
    {
        return 0;
    }
    const auto* block = reinterpret_cast<const BackingAllocation*>(
        static_cast<const std::byte*>(address) - sizeof(BackingAllocation));
    return block->capacity_;
}

namespace
{
MemorySnapshot read_snapshot(bool include_heap_records) noexcept
{
    const std::lock_guard lock(counters_mutex);
#if defined(ENGINE_ENABLE_GENERAL_HEAP_OBSERVATION)
    const heap::Pause pause;
#endif
    auto result = counters;
    result.timestamp_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
            .count());
    result.mode = mode;
    result.connection_id = current_connection();
    result.connected = result.connection_id != 0;
    result.callstacks_available = has_callstacks();
#if defined(ENGINE_ENABLE_GENERAL_HEAP_OBSERVATION)
    result.general_heap = heap::allocations.snapshot(include_heap_records);
#else
    static_cast<void>(include_heap_records);
#endif
    return result;
}
} // namespace

MemorySnapshot snapshot() noexcept
{
    return read_snapshot(true);
}

void publish_plots() noexcept
{
#if defined(ENGINE_ENABLE_TRACY)
    if (!ENGINE_PROFILE_CONNECTED())
    {
        return;
    }
    const auto sample = read_snapshot(false);
    for (std::size_t index = 0; index < names.size(); ++index)
    {
        const auto& pool = sample.pools[index];
        ENGINE_PROFILE_PLOT(names[index].bytes, static_cast<std::int64_t>(pool.current_bytes));
        ENGINE_PROFILE_PLOT(names[index].peak, static_cast<std::int64_t>(pool.peak_bytes));
    }
    ENGINE_PROFILE_PLOT("Memory.captureGaps", static_cast<std::int64_t>(sample.capture_gaps));
    if (sample.general_heap.available)
    {
        const auto& raw = sample.general_heap;
        ENGINE_PROFILE_PLOT("Memory.generalHeapBytes",
                            static_cast<std::int64_t>(raw.unattributed.current_bytes));
        ENGINE_PROFILE_PLOT("Memory.generalHeapPeakBytes",
                            static_cast<std::int64_t>(raw.unattributed.peak_bytes));
        ENGINE_PROFILE_PLOT("Memory.generalHeapCaptureGaps", static_cast<std::int64_t>(raw.capture_gaps));
    }
#endif
}

void begin_startup_capture() noexcept
{
#if defined(ENGINE_ENABLE_MACOS_HEAP_OBSERVATION)
    heap::install_macos_observer();
#endif
#if defined(ENGINE_ENABLE_TRACY)
    const auto* value = std::getenv("ENGINE_TRACY_WAIT_FOR_CAPTURE_MS");
    if (!value || current_connection() != 0)
    {
        return;
    }
    const std::string_view text(value);
    unsigned int wait_ms = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), wait_ms);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || wait_ms > 30000)
    {
        std::fputs("ENGINE_TRACY_WAIT_FOR_CAPTURE_MS must be 0 through 30000\n", stderr);
        return;
    }
    const auto deadline = Clock::now() + std::chrono::milliseconds(wait_ms);
    while (Clock::now() < deadline)
    {
        if (current_connection() != 0)
        {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (wait_ms > 0)
    {
        std::fputs("Tracy startup capture connection timed out\n", stderr);
    }
#endif
}
#else
MemorySnapshot snapshot() noexcept
{
    return {};
}
#endif

} // namespace engine::memory_profile
