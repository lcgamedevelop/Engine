#pragma once

#include "runtime/performance/function_profile.hpp"

#include "runtime/resource/virtual_file_system.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace engine::plugins::script::vm
{

// 只承载原生调用的返回数据；不拥有虚拟机句柄。uint64_t 无损映射为 BigInt。
struct NativeValue
{
    using Array = std::vector<NativeValue>;
    using Object = std::vector<std::pair<std::string, NativeValue>>;
    std::variant<std::monostate, bool, double, std::uint64_t, std::string, Array, Object,
                 ByteBuffer>
        data;

    NativeValue() = default;
    NativeValue(bool value) : data(value)
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
    }
    NativeValue(double value) : data(value)
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
    }
    NativeValue(std::int32_t value) : data(static_cast<double>(value))
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
    }
    NativeValue(std::uint32_t value) : data(static_cast<double>(value))
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
    }
    NativeValue(std::uint64_t value) : data(value)
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
    }
    NativeValue(std::string_view value) : data(std::string(value))
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
    }
    NativeValue(const char* value) : data(std::string(value))
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
    }
    NativeValue(std::string value) : data(std::move(value))
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
    }
    NativeValue(Array value) : data(std::move(value))
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
    }
    NativeValue(Object value) : data(std::move(value))
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
    }
    NativeValue(ByteBuffer value) : data(std::move(value))
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
    }
};

} // namespace engine::plugins::script::vm
