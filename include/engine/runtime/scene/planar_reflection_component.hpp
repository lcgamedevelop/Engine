#pragma once

#include "runtime/performance/function_profile.hpp"

#include "runtime/scene/component.hpp"

namespace engine
{

class PlanarReflectionComponent final : public Component
{
public:
    using Data = schema::PlanarReflectionDataT;
    PlanarReflectionComponent() noexcept = default;
    explicit PlanarReflectionComponent(Component component) noexcept
        : Component(component, component_type)
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
    }
    [[nodiscard]] static bool register_type(ComponentRegistry& registry);
    static constexpr ComponentType component_type = ComponentType::PlanarReflectionData;
    [[nodiscard]] bool restore_data(const schema::ComponentDataT& data);

    [[nodiscard]] bool get_settings(Data& output) const noexcept;
    [[nodiscard]] bool set_settings(const PlanarReflectionSettings& settings) noexcept;
};

} // namespace engine
