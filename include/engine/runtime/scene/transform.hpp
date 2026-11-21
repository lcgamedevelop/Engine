#pragma once

#include "runtime/performance/function_profile.hpp"

#include "runtime/scene/component.hpp"

namespace engine
{

class Transform final : public Component
{
public:
    using Data = schema::TransformDataT;
    Transform() noexcept = default;
    explicit Transform(Component component) noexcept : Component(component, component_type)
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
    }
    [[nodiscard]] static bool register_type(ComponentRegistry& registry);
    static constexpr ComponentType component_type = ComponentType::TransformData;
    [[nodiscard]] bool restore_data(const schema::ComponentDataT& data);

    [[nodiscard]] bool get_settings(Data& output) const noexcept;
    [[nodiscard]] bool set_settings(const TransformSettings& settings) noexcept;
    // 输出 16 个列主序 float；失败时非空输出保持单位矩阵。
    [[nodiscard]] bool world_matrix(double* output) const noexcept;
};

} // namespace engine
