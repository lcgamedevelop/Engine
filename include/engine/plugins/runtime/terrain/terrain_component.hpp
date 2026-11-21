#pragma once

#include "runtime/performance/function_profile.hpp"

#include "plugins/runtime/terrain/terrain_operations.hpp"
#include "runtime/scene/collider.hpp"
#include "runtime/scene/component.hpp"

#include <cstdint>
#include <vector>

namespace engine::plugins::terrain
{
class TerrainSystem;
struct TerrainComponentRuntime;

class TerrainComponent final : public Component
{
public:
    using Data = schema::TerrainDataT;
    using Runtime = TerrainComponentRuntime;
    static constexpr ComponentType component_type = ComponentType::TerrainData;
    TerrainComponent() noexcept = default;
    explicit TerrainComponent(Component component) noexcept : Component(component, component_type)
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
    }
    [[nodiscard]] static bool register_type(ComponentRegistry& registry);
    [[nodiscard]] static bool valid_settings(const Data& settings) noexcept;
    [[nodiscard]] bool get_settings(Data& output) const;
    [[nodiscard]] bool set_settings(const Data& settings);
    [[nodiscard]] bool restore_data(const schema::ComponentDataT& data);
    [[nodiscard]] bool set_view(std::uint64_t view_key, const TerrainView& view);
    [[nodiscard]] bool remove_view(std::uint64_t view_key);
    [[nodiscard]] bool pin_regions(const TerrainPinSpec& spec, TerrainPinId& output);
    [[nodiscard]] bool release_pin(TerrainPinId id);
    [[nodiscard]] bool request_edit(const TerrainEditSpec& spec, TerrainOperationId& output);
    [[nodiscard]] bool request_collapse(const TerrainCollapseSpec& spec,
                                        TerrainOperationId& output);
    [[nodiscard]] bool request_reset(const TerrainResetSpec& spec, TerrainOperationId& output);
    [[nodiscard]] bool request_retire(const TerrainRetireSpec& spec, TerrainOperationId& output);
    [[nodiscard]] bool request_save(const TerrainSaveSpec& spec, TerrainOperationId& output);
    [[nodiscard]] bool request_load(const TerrainLoadSpec& spec, TerrainOperationId& output);
    [[nodiscard]] bool operation_status(TerrainOperationId id,
                                        TerrainOperationSnapshot& output) const;
    [[nodiscard]] bool cancel_operation(TerrainOperationId id);
    [[nodiscard]] bool release_operation(TerrainOperationId id);
    [[nodiscard]] bool query_regions(const TerrainRegionQuery& query,
                                     TerrainRegionSnapshot& output) const;
    [[nodiscard]] bool collect_support(const TerrainRegionQuery& query,
                                       std::vector<Collider>& output) const;

private:
    friend class TerrainSystem;
    template <class T> friend auto engine::ecs_detail::component_runtime(T component) noexcept;
    [[nodiscard]] Runtime* runtime() const noexcept;
};
} // namespace engine::plugins::terrain
