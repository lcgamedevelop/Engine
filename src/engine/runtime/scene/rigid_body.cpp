#include "runtime/scene/rigid_body.hpp"
#include "runtime/performance/profiling.hpp"

#include "runtime/scene/rigid_body_runtime.hpp"

#include "runtime/scene/ecs_storage.hpp"

#include "runtime/scene/collider.hpp"
#include "runtime/scene/node.hpp"

#include <cmath>

namespace engine
{
namespace
{

bool bounded(double value, double minimum, double maximum) noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return std::isfinite(value) && value >= minimum && value <= maximum;
}

bool vector_bounded(const Vec3& value, double limit) noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return bounded(value.x(), -limit, limit) && bounded(value.y(), -limit, limit) &&
           bounded(value.z(), -limit, limit);
}

bool zero(const Vec3& value) noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return value.x() == 0.0 && value.y() == 0.0 && value.z() == 0.0;
}

} // namespace

bool RigidBody::register_type(ComponentRegistry& registry)
{
    ENGINE_PROFILE_FUNCTION();
    return register_component_storage<RigidBody, Data, Runtime>(registry);
}

RigidBody::Runtime* RigidBody::runtime() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return runtime_state<Runtime>();
}

bool RigidBody::valid_settings(const schema::RigidBodyDataT& value) noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return value.motion >= schema::BodyMotion::static_body &&
           value.motion <= schema::BodyMotion::dynamic && bounded(value.mass, 0.0001F, 1000000.0) &&
           bounded(value.gravity_factor, 0.0, 100.0) && bounded(value.linear_damping, 0.0, 1.0) &&
           bounded(value.angular_damping, 0.0, 1.0) &&
           vector_bounded(value.linear_velocity, 10000.0) &&
           vector_bounded(value.angular_velocity, 10000.0) &&
           (value.motion != schema::BodyMotion::static_body ||
            (zero(value.linear_velocity) && zero(value.angular_velocity)));
}

bool RigidBody::get_settings(Data& output) const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    const auto* value = read_data<Data>();
    output = value ? *value : Data{};
    return value != nullptr;
}

bool RigidBody::set_settings(const schema::RigidBodyDataT& value) noexcept
{
    ENGINE_PROFILE_FUNCTION();
    if (!writable())
    {
        return false;
    }
    if (!valid_settings(value))
    {
        return false;
    }
    auto collider = node().get_component<Collider>();
    Collider::Data collider_settings;
    if (value.motion != schema::BodyMotion::static_body && collider &&
        (!collider.get_settings(collider_settings) ||
         collider_settings.shape == schema::ColliderShape::mesh))
    {
        return false;
    }
    *write_data<Data>() = value;
    modified();
    if (value.motion != schema::BodyMotion::dynamic)
    {
        runtime()->pending_impulse_ = {};
        runtime()->synchronized_collider_ = 0;
        runtime()->sleep_requested_ = false;
    }
    return true;
}

bool RigidBody::restore_data(const schema::ComponentDataT& value)
{
    ENGINE_PROFILE_FUNCTION();
    if (!value.flags)
    {
        return false;
    }
    if (!writable())
    {
        return false;
    }
    const auto* settings = value.payload.AsRigidBodyData();
    if (settings == nullptr || !set_settings(*settings))
    {
        return false;
    }
    runtime()->pending_impulse_ = {};
    runtime()->synchronized_collider_ = 0;
    runtime()->sleep_requested_ = false;
    if (!restore_flags(value))
    {
        return false;
    }
    return true;
}

bool RigidBody::add_impulse(const Vec3& value) noexcept
{
    ENGINE_PROFILE_FUNCTION();
    if (!writable())
    {
        return false;
    }
    const auto* data = read_data<Data>();
    if (!data || !active() || data->motion != schema::BodyMotion::dynamic ||
        !vector_bounded(value, 1000000.0))
    {
        return false;
    }
    const Vec3 sum(runtime()->pending_impulse_.x() + value.x(),
                   runtime()->pending_impulse_.y() + value.y(),
                   runtime()->pending_impulse_.z() + value.z());
    if (!vector_bounded(sum, 1000000.0))
    {
        return false;
    }
    runtime()->pending_impulse_ = sum;
    return true;
}

bool RigidBody::set_linear_velocity(const Vec3& value) noexcept
{
    ENGINE_PROFILE_FUNCTION();
    if (!writable())
    {
        return false;
    }
    Data replacement;
    if (!get_settings(replacement))
    {
        return false;
    }
    replacement.linear_velocity = value;
    return set_settings(replacement);
}

Vec3 RigidBody::linear_velocity() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    if (!valid())
    {
        return {};
    }
    const auto* data = read_data<Data>();
    return data ? data->linear_velocity : Vec3{};
}

bool RigidBody::get_sleeping(bool& output) const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    output = false;
    Data data;
    const auto collider = node().get_component<Collider>();
    if (!active() || !get_settings(data) || data.motion != schema::BodyMotion::dynamic ||
        !collider.active() || runtime()->synchronized_collider_ != collider.instance_id())
    {
        return false;
    }
    output = runtime()->sleeping_;
    return true;
}

bool RigidBody::set_sleeping(bool sleeping) noexcept
{
    ENGINE_PROFILE_FUNCTION();
    Data data;
    if (!writable() || !get_settings(data) || data.motion != schema::BodyMotion::dynamic)
    {
        return false;
    }
    runtime()->requested_sleeping_ = sleeping;
    runtime()->sleep_requested_ = true;
    return true;
}

} // namespace engine
