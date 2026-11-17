#pragma once

#include "plugins/runtime/terrain/terrain_fragment.hpp"
#include "plugins/runtime/terrain/terrain_state_store.hpp"
#include "plugins/runtime/terrain/terrain_system.hpp"
#include "runtime/resource/resource_reference.hpp"
#include "runtime/scene/geometry_update.hpp"
#include "runtime/scene/transform.hpp"

#include <atomic>
#include <future>
#include <map>
#include <memory>
#include <set>
#include <vector>

namespace engine::plugins::terrain
{
struct TerrainWork
{
    TerrainReadSnapshot input;
    TerrainDelta delta;
    std::vector<TerrainRegionKey> keys;
    std::vector<TerrainRegionKey> required;
    std::map<TerrainRegionKey, schema::TerrainRegionStateT, TerrainRegionLess> saved_regions;
    std::string saved_slot;
    std::vector<TerrainMeshOutput> meshes;
    std::vector<TerrainFragment> fragments;
    std::set<TerrainRegionKey, TerrainRegionLess> collision_keys;
    std::vector<schema::TerrainBlockDataT> original_blocks;
    std::vector<schema::TerrainBlockDataT> checkpoint_blocks;
    schema::TerrainCheckpointDataT checkpoint;
    std::shared_ptr<TerrainStateStore> store;
    std::string base_guid;
    std::string base_hash;
    std::shared_ptr<std::atomic<bool>> cancelled = std::make_shared<std::atomic<bool>>(false);
    std::string error;
};

struct TerrainOperation
{
    ~TerrainOperation() noexcept;
    struct Load
    {
        std::unique_ptr<ResourceLoad> request;
        ResourceReference resource;
    };
    TerrainRequest request;
    TerrainOperationSnapshot snapshot;
    std::shared_ptr<TerrainWork> work;
    std::future<bool> future;
    std::unique_ptr<GeometryUpdate> geometry;
    std::map<TerrainRegionKey, Load, TerrainRegionLess> loads;
    std::vector<TerrainRegionKey> required;
    std::vector<TerrainRegionKey> retired;
    std::map<TerrainRegionKey, Node, TerrainRegionLess> prepared_nodes;
    std::vector<Node> prepared_fragments;
    std::vector<std::uint64_t> retire_fragments;
    TerrainVolume candidate_volume;
    TransformSettings pose;
    GeometryUpdateSpec geometry_spec;
    WorldSystem* scope_world = nullptr;
    SceneScopeId new_scope;
    std::uint64_t settings_revision = 0;
    std::uint64_t demand_revision = 0;
    std::size_t phase = 0;
    std::size_t next_mesh = 0;
    bool initial = false;
    bool residency_only = false;
    bool replace_source = false;
    bool reconfigure = false;
    bool cancelled = false;
    bool persistence_commit = false;
    bool discard_when_terminal = false;
};

[[nodiscard]] inline bool terminal(TerrainOperationState state) noexcept
{
    return state == TerrainOperationState::completed || state == TerrainOperationState::cancelled ||
           state == TerrainOperationState::failed;
}
} // namespace engine::plugins::terrain
