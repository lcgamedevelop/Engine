#pragma once

#include <cstddef>
#include <cstdint>

namespace engine::memory_profile::trace
{
inline constexpr char general_heap_pool[] = "Engine.GeneralHeap";

[[nodiscard]] std::uint64_t allocate(const char* pool, const void* address,
                                     std::size_t bytes) noexcept;
[[nodiscard]] bool release(const char* pool, const void* address,
                           std::uint64_t connection) noexcept;
} // namespace engine::memory_profile::trace
