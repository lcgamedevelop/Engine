#include "plugins/runtime/character_controller/character_visual.hpp"
#include "runtime/performance/profiling.hpp"

#include "runtime/scene/ecs_storage.hpp"

namespace engine::plugins::character_controller
{
bool CharacterVisual::register_type(ComponentRegistry& registry)
{
    ENGINE_PROFILE_FUNCTION();
    return register_component_storage<CharacterVisual, Data>(registry);
}

bool CharacterVisual::valid_settings(const schema::CharacterVisualDataT& value) noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return value.kind >= schema::CharacterVisualKind::MIN &&
           value.kind <= schema::CharacterVisualKind::MAX;
}

bool CharacterVisual::get_settings(Data& output) const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    const auto* value = read_data<Data>();
    output = value ? *value : Data{};
    return value != nullptr;
}

bool CharacterVisual::set_settings(const schema::CharacterVisualDataT& value) noexcept
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
    *write_data<Data>() = value;
    modified();
    return true;
}

bool CharacterVisual::restore_data(const schema::ComponentDataT& value)
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
    const auto* settings = value.payload.AsCharacterVisualData();
    if (settings == nullptr || !set_settings(*settings))
    {
        return false;
    }
    if (!restore_flags(value))
    {
        return false;
    }
    return true;
}

} // namespace engine::plugins::character_controller
