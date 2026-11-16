#pragma once

#include "runtime/scene/component_runtime.hpp"
#include "runtime/scene/rigid_body.hpp"

namespace engine
{
struct RigidBodyRuntime
{
    Vec3 pending_impulse_{};
    std::uint64_t synchronized_collider_ = 0;
    bool sleeping_ = false;
    bool sleep_requested_ = false;
    bool requested_sleeping_ = false;
};
} // namespace engine
