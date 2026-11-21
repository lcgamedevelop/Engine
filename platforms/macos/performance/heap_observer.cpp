#include "macos/performance/heap_hooks.hpp"

#include "runtime/performance/heap_profiling.hpp"
#include "runtime/performance/profiling.hpp"

#include <cstdio>
#include <cstdlib>

namespace engine::memory_profile::heap
{
namespace
{
void record_allocation(const void* address, std::size_t bytes, const void* zone) noexcept
{
    allocations.allocated(address, bytes, zone);
}

void record_free(const void* address) noexcept
{
    allocations.freed(address);
}

void* record_resize(void* address, std::size_t bytes, RawReallocate raw, const void* argument,
                    bool releases_on_failure, const void* zone) noexcept
{
    return allocations.reallocate(address, bytes, raw, argument, releases_on_failure, zone);
}

void record_zone_destroy(const void* zone) noexcept
{
    allocations.destroy_zone(zone);
}

constexpr macos::HeapHooks hooks{enter, leave, record_allocation, record_free, record_resize,
                                 record_zone_destroy};

bool start_observer() noexcept
{
    ENGINE_PROFILE_FUNCTION();
    if (!initialize_thread_guard())
    {
        std::fputs("Could not initialize the macOS heap observer thread guard\n", stderr);
        return false;
    }
    if (std::atexit([] { engine_macos_set_heap_hooks(nullptr); }) != 0)
    {
        std::fputs("Could not register the macOS heap observer shutdown\n", stderr);
        return false;
    }
    allocations.start();
    engine_macos_set_heap_hooks(&hooks);
    return true;
}
} // namespace

void install_macos_observer() noexcept
{
    ENGINE_PROFILE_FUNCTION();
    static const auto started = start_observer();
    static_cast<void>(started);
}
} // namespace engine::memory_profile::heap
