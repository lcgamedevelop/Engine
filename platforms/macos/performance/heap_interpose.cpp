#include "macos/performance/heap_hooks.hpp"

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <limits>
#include <malloc/malloc.h>

namespace
{
using engine::memory_profile::macos::HeapHooks;

constinit std::atomic<const HeapHooks*> installed_hooks{nullptr};

class Observation final
{
public:
    Observation() noexcept : hooks_(installed_hooks.load(std::memory_order_acquire))
    {
        if (hooks_ && !hooks_->enter())
        {
            hooks_ = nullptr;
        }
    }

    ~Observation()
    {
        if (hooks_)
        {
            hooks_->leave();
        }
    }

    Observation(const Observation&) = delete;
    Observation& operator=(const Observation&) = delete;

    [[nodiscard]] const HeapHooks* hooks() const noexcept
    {
        return hooks_;
    }

private:
    const HeapHooks* hooks_;
};

template<class Allocate>
void* allocate(std::size_t bytes, malloc_zone_t* zone, Allocate raw) noexcept
{
    const Observation observation;
    auto* address = raw();
    if (address && observation.hooks())
    {
        const int saved_errno = errno;
        observation.hooks()->allocated(address, bytes, zone);
        errno = saved_errno;
    }
    return address;
}

template<class Allocate>
void* allocate_zeroed(std::size_t count, std::size_t size, malloc_zone_t* zone,
                      Allocate raw) noexcept
{
    if (size && count > std::numeric_limits<std::size_t>::max() / size)
    {
        return raw();
    }
    return allocate(count * size, zone, raw);
}

template<class Allocate>
int allocate_aligned(void** output, std::size_t bytes, Allocate raw) noexcept
{
    int status = 0;
    (void)allocate(bytes, nullptr, [&]
    {
        status = raw();
        return status == 0 ? *output : nullptr;
    });
    return status;
}

template<class Release>
void release(void* address, Release raw) noexcept
{
    const Observation observation;
    if (address && observation.hooks())
    {
        const int saved_errno = errno;
        observation.hooks()->freed(address);
        errno = saved_errno;
    }
    raw();
}

template<class Resize>
void* resize(void* address, std::size_t bytes, malloc_zone_t* zone,
             bool releases_on_failure, Resize raw) noexcept
{
    const auto* hooks = installed_hooks.load(std::memory_order_acquire);
    if (!hooks)
    {
        return raw(address, bytes);
    }
    const auto call = [](void* original, std::size_t size, const void* argument) noexcept
    {
        return (*static_cast<const Resize*>(argument))(original, size);
    };
    return hooks->reallocate(address, bytes, call, &raw, releases_on_failure, zone);
}

void* observe_malloc(std::size_t bytes) noexcept
{
    return allocate(bytes, nullptr, [=] { return ::malloc(bytes); });
}

void* observe_calloc(std::size_t count, std::size_t size) noexcept
{
    return allocate_zeroed(count, size, nullptr, [=] { return ::calloc(count, size); });
}

void* observe_realloc(void* address, std::size_t bytes) noexcept
{
    return resize(address, bytes, nullptr, false, ::realloc);
}

void* observe_reallocf(void* address, std::size_t bytes) noexcept
{
    return resize(address, bytes, nullptr, true, ::reallocf);
}

void observe_free(void* address) noexcept
{
    release(address, [=] { ::free(address); });
}

void* observe_valloc(std::size_t bytes) noexcept
{
    return allocate(bytes, nullptr, [=] { return ::valloc(bytes); });
}

void* observe_aligned_alloc(std::size_t alignment, std::size_t bytes) noexcept
{
    return allocate(bytes, nullptr, [=] { return ::aligned_alloc(alignment, bytes); });
}

int observe_posix_memalign(void** output, std::size_t alignment, std::size_t bytes) noexcept
{
    return allocate_aligned(output, bytes, [=]
    {
        return ::posix_memalign(output, alignment, bytes);
    });
}

void* observe_malloc_zone_malloc(malloc_zone_t* zone, std::size_t bytes) noexcept
{
    return allocate(bytes, zone, [=] { return ::malloc_zone_malloc(zone, bytes); });
}

void* observe_malloc_zone_calloc(malloc_zone_t* zone, std::size_t count,
                                 std::size_t size) noexcept
{
    return allocate_zeroed(count, size, zone, [=]
    {
        return ::malloc_zone_calloc(zone, count, size);
    });
}

void* observe_malloc_zone_realloc(malloc_zone_t* zone, void* address, std::size_t bytes) noexcept
{
    return resize(address, bytes, zone, false, [=](void* ptr, std::size_t size)
    {
        return ::malloc_zone_realloc(zone, ptr, size);
    });
}

void observe_malloc_zone_free(malloc_zone_t* zone, void* address) noexcept
{
    release(address, [=] { ::malloc_zone_free(zone, address); });
}

void* observe_malloc_zone_valloc(malloc_zone_t* zone, std::size_t bytes) noexcept
{
    return allocate(bytes, zone, [=] { return ::malloc_zone_valloc(zone, bytes); });
}

void* observe_malloc_zone_memalign(malloc_zone_t* zone, std::size_t alignment,
                                   std::size_t bytes) noexcept
{
    return allocate(bytes, zone, [=] { return ::malloc_zone_memalign(zone, alignment, bytes); });
}

unsigned observe_malloc_zone_batch_malloc(malloc_zone_t* zone, std::size_t bytes, void** addresses,
                                           unsigned requested) noexcept
{
    const Observation observation;
    const auto count = ::malloc_zone_batch_malloc(zone, bytes, addresses, requested);
    if (const auto* hooks = observation.hooks())
    {
        const int saved_errno = errno;
        for (unsigned index = 0; index < count; ++index)
        {
            hooks->allocated(addresses[index], bytes, zone);
        }
        errno = saved_errno;
    }
    return count;
}

void observe_malloc_zone_batch_free(malloc_zone_t* zone, void** addresses, unsigned count) noexcept
{
    const Observation observation;
    if (const auto* hooks = observation.hooks())
    {
        const int saved_errno = errno;
        for (unsigned index = 0; index < count; ++index)
        {
            if (addresses[index])
            {
                hooks->freed(addresses[index]);
            }
        }
        errno = saved_errno;
    }
    ::malloc_zone_batch_free(zone, addresses, count);
}

void observe_malloc_destroy_zone(malloc_zone_t* zone) noexcept
{
    const Observation observation;
    if (zone && observation.hooks())
    {
        const int saved_errno = errno;
        observation.hooks()->destroy_zone(zone);
        errno = saved_errno;
    }
    ::malloc_destroy_zone(zone);
}

#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 140000
void* observe_malloc_type_malloc(std::size_t bytes, malloc_type_id_t type) noexcept
{
    if (__builtin_available(macOS 14.0, *))
    {
        return allocate(bytes, nullptr, [=] { return ::malloc_type_malloc(bytes, type); });
    }
    return observe_malloc(bytes);
}

void* observe_malloc_type_calloc(std::size_t count, std::size_t size,
                                 malloc_type_id_t type) noexcept
{
    if (__builtin_available(macOS 14.0, *))
    {
        return allocate_zeroed(count, size, nullptr, [=]
        {
            return ::malloc_type_calloc(count, size, type);
        });
    }
    return observe_calloc(count, size);
}

void* observe_malloc_type_realloc(void* address, std::size_t bytes, malloc_type_id_t type) noexcept
{
    if (__builtin_available(macOS 14.0, *))
    {
        return resize(address, bytes, nullptr, false, [=](void* ptr, std::size_t size)
        {
            return ::malloc_type_realloc(ptr, size, type);
        });
    }
    return observe_realloc(address, bytes);
}

void observe_malloc_type_free(void* address, malloc_type_id_t type) noexcept
{
    if (__builtin_available(macOS 14.0, *))
    {
        release(address, [=] { ::malloc_type_free(address, type); });
        return;
    }
    observe_free(address);
}

void* observe_malloc_type_valloc(std::size_t bytes, malloc_type_id_t type) noexcept
{
    if (__builtin_available(macOS 14.0, *))
    {
        return allocate(bytes, nullptr, [=] { return ::malloc_type_valloc(bytes, type); });
    }
    return observe_valloc(bytes);
}

void* observe_malloc_type_aligned_alloc(std::size_t alignment, std::size_t bytes,
                                        malloc_type_id_t type) noexcept
{
    if (__builtin_available(macOS 14.0, *))
    {
        return allocate(bytes, nullptr, [=]
        {
            return ::malloc_type_aligned_alloc(alignment, bytes, type);
        });
    }
    return observe_aligned_alloc(alignment, bytes);
}

int observe_malloc_type_posix_memalign(void** output, std::size_t alignment, std::size_t bytes,
                                       malloc_type_id_t type) noexcept
{
    if (__builtin_available(macOS 14.0, *))
    {
        return allocate_aligned(output, bytes, [=]
        {
            return ::malloc_type_posix_memalign(output, alignment, bytes, type);
        });
    }
    return observe_posix_memalign(output, alignment, bytes);
}

#endif

struct Interposition
{
    const void* replacement;
    const void* original;
};

// dyld leaves calls from this image bound to the original system functions.
#define ENGINE_HEAP_INTERPOSE(symbol)                                                             \
    [[gnu::used, gnu::section("__DATA,__interpose")]] const Interposition interpose_##symbol{      \
        reinterpret_cast<const void*>(&observe_##symbol), reinterpret_cast<const void*>(&::symbol)}

ENGINE_HEAP_INTERPOSE(malloc);
ENGINE_HEAP_INTERPOSE(calloc);
ENGINE_HEAP_INTERPOSE(realloc);
ENGINE_HEAP_INTERPOSE(reallocf);
ENGINE_HEAP_INTERPOSE(free);
ENGINE_HEAP_INTERPOSE(valloc);
ENGINE_HEAP_INTERPOSE(aligned_alloc);
ENGINE_HEAP_INTERPOSE(posix_memalign);
ENGINE_HEAP_INTERPOSE(malloc_zone_malloc);
ENGINE_HEAP_INTERPOSE(malloc_zone_calloc);
ENGINE_HEAP_INTERPOSE(malloc_zone_realloc);
ENGINE_HEAP_INTERPOSE(malloc_zone_free);
ENGINE_HEAP_INTERPOSE(malloc_zone_valloc);
ENGINE_HEAP_INTERPOSE(malloc_zone_memalign);
ENGINE_HEAP_INTERPOSE(malloc_zone_batch_malloc);
ENGINE_HEAP_INTERPOSE(malloc_zone_batch_free);
ENGINE_HEAP_INTERPOSE(malloc_destroy_zone);

// The tuples hold weak imports for entry points introduced after the deployment target.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunguarded-availability-new"
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 140000
ENGINE_HEAP_INTERPOSE(malloc_type_malloc);
ENGINE_HEAP_INTERPOSE(malloc_type_calloc);
ENGINE_HEAP_INTERPOSE(malloc_type_realloc);
ENGINE_HEAP_INTERPOSE(malloc_type_free);
ENGINE_HEAP_INTERPOSE(malloc_type_valloc);
ENGINE_HEAP_INTERPOSE(malloc_type_aligned_alloc);
ENGINE_HEAP_INTERPOSE(malloc_type_posix_memalign);
#endif
#pragma clang diagnostic pop
#undef ENGINE_HEAP_INTERPOSE
} // namespace

extern "C" [[gnu::visibility("default")]] void engine_macos_set_heap_hooks(
    const engine::memory_profile::macos::HeapHooks* hooks) noexcept
{
    installed_hooks.store(hooks, std::memory_order_release);
}
