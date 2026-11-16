#pragma once

#include "plugins/runtime/terrain/terrain_volume.hpp"
#include "runtime/scene/scene_loading.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine::plugins::terrain
{
// The generated view fields are also the runtime view contract; no duplicate persistent model.
using TerrainView = schema::TerrainViewDataT;

struct TerrainRegionStatus
{
    TerrainRegionKey key;
    std::uint64_t density_revision = 0;
    std::uint64_t mesh_revision = 0;
    std::uint64_t collision_revision = 0;
    bool collision_required = false;
    bool known = false;
    bool dirty = false;
    std::uint32_t pin_count = 0;
    SceneWorkState state = SceneWorkState::queued;
    std::string blocked_reason;
};

struct TerrainPinSpec
{
    std::vector<TerrainRegionKey> regions;
    bool collision_required = false;
    std::string reason;
    std::uint64_t owner_key = 0;
};

struct TerrainPlanInput
{
    std::shared_ptr<const schema::TerrainAssetDataT> source;
    std::vector<TerrainView> views;
    std::vector<TerrainPinSpec> pins;
    std::vector<TerrainRegionStatus> resident;
    schema::TerrainBudgetDataT budget;
};

struct TerrainRegionPlan
{
    std::vector<TerrainRegionKey> acquire;
    std::vector<TerrainRegionKey> retain;
    std::vector<TerrainRegionKey> retire;
};

class TerrainRegionPlanner
{
public:
    virtual ~TerrainRegionPlanner() noexcept = default;
    [[nodiscard]] virtual bool plan(const TerrainPlanInput& input, TerrainRegionPlan& output,
                                    std::string& error) const = 0;
};

[[nodiscard]] std::unique_ptr<TerrainRegionPlanner>
create_terrain_region_planner(std::string& error);
} // namespace engine::plugins::terrain
