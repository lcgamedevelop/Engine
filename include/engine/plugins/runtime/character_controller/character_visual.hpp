#pragma once

#include "runtime/performance/function_profile.hpp"

#include "runtime/scene/component.hpp"

namespace engine::plugins::character_controller
{

class CharacterVisual final : public Component
{
public:
    using Data = schema::CharacterVisualDataT;
    CharacterVisual() noexcept = default;
    explicit CharacterVisual(Component component) noexcept : Component(component, component_type)
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
    }
    [[nodiscard]] static bool register_type(ComponentRegistry& registry);
    static constexpr ComponentType component_type = ComponentType::CharacterVisualData;
    [[nodiscard]] static bool valid_settings(const schema::CharacterVisualDataT& settings) noexcept;
    [[nodiscard]] bool get_settings(Data& output) const noexcept;
    [[nodiscard]] bool set_settings(const schema::CharacterVisualDataT& settings) noexcept;
    [[nodiscard]] bool restore_data(const schema::ComponentDataT& data);
};

} // namespace engine::plugins::character_controller
