#pragma once

#include "runtime/performance/function_profile.hpp"

// Internal component access; the returned ECS column borrow ends at structural mutation.
#include "runtime/scene/component.hpp"

namespace engine::ecs_detail
{
template <class T> auto component_runtime(T component) noexcept
{
    ENGINE_PROFILE_FUNCTION_PROBE();
    return component.runtime();
}
} // namespace engine::ecs_detail
