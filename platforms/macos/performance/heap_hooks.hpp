#pragma once

#include <cstddef>

namespace engine::memory_profile::macos
{
using Resize = void* (*)(void*, std::size_t, const void*) noexcept;

// Installed by the host. The adapter borrows this table until process exit.
struct HeapHooks
{
    bool (*enter)() noexcept;
    void (*leave)() noexcept;
    void (*allocated)(const void*, std::size_t, const void*) noexcept;
    void (*freed)(const void*) noexcept;
    void* (*reallocate)(void*, std::size_t, Resize, const void*, bool, const void*) noexcept;
    void (*destroy_zone)(const void*) noexcept;
};
} // namespace engine::memory_profile::macos

extern "C" void engine_macos_set_heap_hooks(
    const engine::memory_profile::macos::HeapHooks* hooks) noexcept;
