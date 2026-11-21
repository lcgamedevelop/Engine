#pragma once

#if defined(ENGINE_ENABLE_FUNCTION_PROBES)

#include <atomic>
#include <cstddef>
#include <source_location>
#include <type_traits>

namespace engine::profile_detail
{
inline constexpr std::size_t function_zone_storage_size = 32;
using FunctionZoneEnd = void (*)(void*) noexcept;
using FunctionZoneBegin = FunctionZoneEnd (*)(void*, std::source_location) noexcept;
inline constinit std::atomic<FunctionZoneBegin> function_zone_begin{};

class FunctionProfileScope final
{
public:
    constexpr explicit FunctionProfileScope(
        std::source_location location = std::source_location::current()) noexcept
    {
        if (std::is_constant_evaluated())
        {
            return;
        }
        if (const auto begin = function_zone_begin.load(std::memory_order_acquire))
        {
            end_ = begin(storage_, location);
        }
    }

    constexpr ~FunctionProfileScope() noexcept
    {
        if (!std::is_constant_evaluated() && end_ != nullptr)
        {
            end_(storage_);
        }
    }

    FunctionProfileScope(const FunctionProfileScope&) = delete;
    FunctionProfileScope& operator=(const FunctionProfileScope&) = delete;

private:
    alignas(std::max_align_t) std::byte storage_[function_zone_storage_size]{};
    FunctionZoneEnd end_ = nullptr;
};
} // namespace engine::profile_detail

#endif

#if defined(ENGINE_ENABLE_FUNCTION_PROBES)
#define ENGINE_PROFILE_FUNCTION_PROBE() \
    [[maybe_unused]] const ::engine::profile_detail::FunctionProfileScope engine_function_scope{}
#else
#define ENGINE_PROFILE_FUNCTION_PROBE() static_cast<void>(0)
#endif
