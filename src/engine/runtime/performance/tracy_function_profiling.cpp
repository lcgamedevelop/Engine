#include "runtime/performance/function_profile.hpp"

#include <tracy/Tracy.hpp>

#include <cstdint>
#include <cstring>
#include <memory>

namespace engine::profile_detail
{
namespace
{
static_assert(sizeof(tracy::ScopedZone) <= function_zone_storage_size);
static_assert(alignof(tracy::ScopedZone) <= alignof(std::max_align_t));

FunctionZoneEnd begin_function_zone(void* storage, std::source_location location) noexcept
{
    if (!TracyIsConnected)
    {
        return nullptr;
    }
    const char* file = location.file_name();
    const char* function = location.function_name();
    std::construct_at(static_cast<tracy::ScopedZone*>(storage),
                      static_cast<std::uint32_t>(location.line()), file, std::strlen(file),
                      function, std::strlen(function), function, std::strlen(function), 0U);
    return [](void* zone) noexcept { std::destroy_at(static_cast<tracy::ScopedZone*>(zone)); };
}

[[maybe_unused]] const bool function_profiler_registered = []() noexcept
{
    function_zone_begin.store(begin_function_zone, std::memory_order_release);
    return true;
}();
} // namespace
} // namespace engine::profile_detail
