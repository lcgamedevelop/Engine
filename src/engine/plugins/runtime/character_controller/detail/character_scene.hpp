#pragma once

#include "runtime/performance/function_profile.hpp"

#include "runtime/scene/transform.hpp"

namespace engine::plugins::character_controller::detail
{

[[nodiscard]] inline bool active(const Node& node) noexcept
{
    ENGINE_PROFILE_FUNCTION_PROBE();
    for (Node current = node; static_cast<bool>(current); current = current.parent())
    {
        const auto transform = current.get_component<Transform>();
        if (!transform || !transform.enabled())
        {
            return false;
        }
    }
    return true;
}

} // namespace engine::plugins::character_controller::detail
