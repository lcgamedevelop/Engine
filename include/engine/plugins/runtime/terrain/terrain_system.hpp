#pragma once

#include "plugins/runtime/terrain/terrain_component.hpp"
#include "plugins/runtime/terrain/terrain_mesher.hpp"
#include "runtime/scene/scene_preparation.hpp"
#include "runtime/system/system.hpp"

#include <map>
#include <memory>
#include <variant>

namespace engine
{
class WorldSystem;
class ResourceLoader;
class TaskSystem;
class RenderSystem;
class PhysicsSystem;
} // namespace engine

namespace engine::plugins::terrain
{
using TerrainRequest = std::variant<TerrainEditSpec, TerrainCollapseSpec, TerrainResetSpec,
                                    TerrainRetireSpec, TerrainSaveSpec, TerrainLoadSpec>;

class TerrainSystem final : public System, public ScenePreparation
{
public:
    TerrainSystem(WorldSystem& world, ResourceLoader& resources, TaskSystem& tasks,
                  RenderSystem& renderer, PhysicsSystem& physics,
                  std::unique_ptr<TerrainRegionPlanner> planner,
                  std::unique_ptr<TerrainMesher> mesher) noexcept;
    ~TerrainSystem() noexcept override;
    [[nodiscard]] bool synchronize();
    [[nodiscard]] SceneWorkState prepare_scope(SceneScopeId scope,
                                               const SceneWorkBudget& budget) override;
    [[nodiscard]] bool scope_status(SceneScopeId scope,
                                    ScenePreparationSnapshot& output) const override;
    void release_scope(SceneScopeId scope) noexcept override;

protected:
    [[nodiscard]] bool on_initialize(const EngineConfig& config) override;
    [[nodiscard]] bool on_tick(double delta_time) override;
    void on_shutdown() noexcept override;

private:
    friend class TerrainComponent;
    [[nodiscard]] bool request(TerrainComponent component, TerrainRequest request,
                               TerrainOperationId& output);
    [[nodiscard]] bool update(TerrainComponent component, SceneScopeId scope,
                              const SceneWorkBudget& budget);
    WorldSystem& world_;
    ResourceLoader& resources_;
    TaskSystem& tasks_;
    RenderSystem& renderer_;
    PhysicsSystem& physics_;
    std::unique_ptr<TerrainRegionPlanner> planner_;
    std::shared_ptr<TerrainMesher> mesher_;
    std::map<SceneScopeId, ScenePreparationSnapshot> scopes_;
    double simulation_time_ = 0;
};
} // namespace engine::plugins::terrain
