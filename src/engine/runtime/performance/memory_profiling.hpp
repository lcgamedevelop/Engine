#pragma once

#include "runtime/performance/memory_snapshot.hpp"

namespace engine::memory_profile
{

// The buffer owner tracks its existing allocation before publishing it to another thread.
// Destroy or reset the token before freeing the buffer or changing its capacity.
class Allocation
{
public:
    Allocation() noexcept = default;
    Allocation(const Allocation&) = delete;
    Allocation& operator=(const Allocation&) = delete;

#if defined(ENGINE_ENABLE_MEMORY_PROFILING)
    Allocation(Allocation&& other) noexcept;
    Allocation& operator=(Allocation&& other) noexcept;
    ~Allocation();
    void track(Pool pool, const void* address, std::size_t capacity) noexcept;
    void reset() noexcept;

private:
    const void* address_ = nullptr;
    std::size_t capacity_ = 0;
    std::uint64_t connection_id_ = 0;
    std::uint64_t heap_allocation_id_ = 0;
    Pool pool_ = Pool::resource_read;
#else
    Allocation(Allocation&&) noexcept = default;
    Allocation& operator=(Allocation&&) noexcept = default;
    void track(Pool, const void*, std::size_t) noexcept
    {
    }
    void reset() noexcept
    {
    }
#endif
};

#if defined(ENGINE_ENABLE_MEMORY_PROFILING)
// Callers pair these functions using the same pool. The backing allocator remains libc.
class alignas(std::max_align_t) BackingAllocation
{
public:
    [[nodiscard]] static void* allocate_block(Pool pool, std::size_t capacity,
                                              bool zeroed = false) noexcept;
    [[nodiscard]] static void* allocate_aligned_block(Pool pool, std::size_t capacity,
                                                      std::size_t alignment) noexcept;
    [[nodiscard]] static void* reallocate_block(Pool pool, void* address, std::size_t capacity,
                                                std::size_t alignment = 0) noexcept;
    static void free_block(Pool pool, void* address) noexcept;
    [[nodiscard]] static std::size_t capacity(const void* address) noexcept;

private:
    [[nodiscard]] static void* allocate(Pool pool, std::size_t capacity, std::size_t alignment,
                                        bool zeroed) noexcept;
    [[nodiscard]] static BackingAllocation* header(void* address) noexcept;

    std::size_t capacity_;
    std::uint64_t connection_id_;
    std::size_t offset_;
};

void publish_plots() noexcept;
void begin_startup_capture() noexcept;
#else
inline void publish_plots() noexcept
{
}
inline void begin_startup_capture() noexcept
{
}
#endif

} // namespace engine::memory_profile
