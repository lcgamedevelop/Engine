#pragma once

#include "runtime/performance/function_profile.hpp"
#include "plugins/runtime/character_controller/character_types.hpp"
#include <cmath>
namespace engine::plugins::character_controller::detail
{
inline bool bounded(double v, double low, double high)
{
    ENGINE_PROFILE_FUNCTION_PROBE();
    return std::isfinite(v) && v >= low && v <= high;
}
inline bool action_exists(const schema::CharacterControllerDataT& d, std::string_view name)
{
    ENGINE_PROFILE_FUNCTION_PROBE();
    for (const auto& a : d.actions)
    {
        if (a.name == name)
        {
            return true;
        }
    }
    return false;
}
inline bool invalid(std::string& error, std::string message)
{
    ENGINE_PROFILE_FUNCTION_PROBE();
    error = std::move(message);
    return false;
}
} // namespace engine::plugins::character_controller::detail
