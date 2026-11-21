#include "runtime/scene/planar_reflection_component.hpp"
#include "runtime/performance/profiling.hpp"

#include "runtime/scene/ecs_storage.hpp"

#include "runtime/scene/scene_settings_validation.hpp"

namespace engine
{

bool PlanarReflectionComponent::register_type(ComponentRegistry& registry)
{
    ENGINE_PROFILE_FUNCTION();
    return register_component_storage<PlanarReflectionComponent, Data>(registry);
}

bool PlanarReflectionComponent::get_settings(Data& output) const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    const auto* value = read_data<Data>();
    output = value ? *value : Data{};
    return value != nullptr;
}

bool PlanarReflectionComponent::set_settings(const PlanarReflectionSettings& settings) noexcept
{
    ENGINE_PROFILE_FUNCTION();
    if (!writable())
    {
        return false;
    }
    if (!scene_detail::valid_planar_reflection(settings))
    {
        return false;
    }
    *write_data<Data>() = settings;
    modified();
    return true;
}

bool PlanarReflectionComponent::restore_data(const schema::ComponentDataT& data)
{
    ENGINE_PROFILE_FUNCTION();
    if (!data.flags)
    {
        return false;
    }
    if (!writable())
    {
        return false;
    }
    const auto* settings = data.payload.AsPlanarReflectionData();
    if (settings == nullptr || !set_settings(*settings))
    {
        return false;
    }
    if (!restore_flags(data))
    {
        return false;
    }
    return true;
}

} // namespace engine
