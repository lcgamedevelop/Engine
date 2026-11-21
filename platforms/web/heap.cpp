#include "runtime/performance/heap_profiling.hpp"

#include <cerrno>
#include <cstddef>
#include <limits>

extern "C"
{
void* __real_malloc(std::size_t bytes);
void* __real_calloc(std::size_t count, std::size_t size);
void* __real_realloc(void* address, std::size_t bytes);
void* __real_realloc_in_place(void* address, std::size_t bytes);
void __real_free(void* address);
void* __real_aligned_alloc(std::size_t alignment, std::size_t bytes);
int __real_posix_memalign(void** output, std::size_t alignment, std::size_t bytes);
void* __real_memalign(std::size_t alignment, std::size_t bytes);
void* __real_valloc(std::size_t bytes);
void* __real_pvalloc(std::size_t bytes);
}

namespace
{
namespace heap = engine::memory_profile::heap;

template<class Allocate>
void* observe_allocation(std::size_t bytes, Allocate allocate) noexcept
{
    if (!heap::allocations.observing())
    {
        return allocate();
    }
    const heap::Pause pause;
    auto* address = allocate();
    const int saved_errno = errno;
    if (address)
    {
        heap::allocations.allocated(address, bytes);
    }
    errno = saved_errno;
    return address;
}
} // namespace

extern "C" void* __wrap_malloc(std::size_t bytes) noexcept
{
    return observe_allocation(bytes, [=] { return __real_malloc(bytes); });
}

extern "C" void* __wrap_calloc(std::size_t count, std::size_t size) noexcept
{
    if (size && count > std::numeric_limits<std::size_t>::max() / size)
    {
        return __real_calloc(count, size);
    }
    return observe_allocation(count * size, [=] { return __real_calloc(count, size); });
}

extern "C" void* __wrap_realloc(void* address, std::size_t bytes) noexcept
{
    return heap::allocations.reallocate(address, bytes, [](void* ptr, std::size_t size,
                                                           const void*) noexcept
    {
        return __real_realloc(ptr, size);
    }, nullptr);
}

extern "C" void* __wrap_realloc_in_place(void* address, std::size_t bytes) noexcept
{
    return heap::allocations.reallocate(address, bytes, [](void* ptr, std::size_t size,
                                                           const void*) noexcept
    {
        return __real_realloc_in_place(ptr, size);
    }, nullptr);
}

extern "C" void __wrap_free(void* address) noexcept
{
    if (!address || !heap::allocations.observing())
    {
        __real_free(address);
        return;
    }
    const heap::Pause pause;
    const int saved_errno = errno;
    heap::allocations.freed(address);
    errno = saved_errno;
    __real_free(address);
}

extern "C" void* __wrap_aligned_alloc(std::size_t alignment, std::size_t bytes) noexcept
{
    return observe_allocation(bytes, [=] { return __real_aligned_alloc(alignment, bytes); });
}

extern "C" int __wrap_posix_memalign(void** output, std::size_t alignment,
                                     std::size_t bytes) noexcept
{
    int status = 0;
    (void)observe_allocation(bytes, [&]
    {
        status = __real_posix_memalign(output, alignment, bytes);
        return status == 0 ? *output : nullptr;
    });
    return status;
}

extern "C" void* __wrap_memalign(std::size_t alignment, std::size_t bytes) noexcept
{
    return observe_allocation(bytes, [=] { return __real_memalign(alignment, bytes); });
}

extern "C" void* __wrap_valloc(std::size_t bytes) noexcept
{
    return observe_allocation(bytes, [=] { return __real_valloc(bytes); });
}

extern "C" void* __wrap_pvalloc(std::size_t bytes) noexcept
{
    return observe_allocation(bytes, [=] { return __real_pvalloc(bytes); });
}
