#pragma once

#include "plugins/runtime/terrain/terrain_operation.hpp"
#include "runtime/scene/component_runtime.hpp"

#include <map>
#include <set>

namespace engine::plugins::terrain
{
struct TerrainGenerated
{
    std::uint64_t owner = 0;
    std::uint64_t instance = 0;
};

struct TerrainComponentRuntime
{
    struct Region
    {
        TerrainRegionStatus status;
        Node node;
    };
    struct Fragment
    {
        std::uint64_t stable_id = 0;
        TerrainRegionKey source_region;
        Node node;
        double published_at = 0;
        float density = 1600;
        TerrainReadSnapshot samples;
        std::vector<TerrainRegionKey> source_regions;
        Bounds3 bounds;
    };
    TerrainSystem* system_ = nullptr;
    TerrainVolume volume_;
    ResourceReference source_;
    ResourceReference material_;
    std::unique_ptr<ResourceLoad> source_loading_;
    std::unique_ptr<ResourceLoad> material_loading_;
    ResourceReference pending_source_;
    ResourceReference pending_material_;
    std::uint64_t loading_settings_revision_ = 0;
    schema::TerrainDataT active_settings_;
    std::map<std::uint64_t, TerrainView> views_;
    std::map<std::uint64_t, TerrainPinSpec> pins_;
    std::map<std::uint64_t, std::unique_ptr<TerrainOperation>> operations_;
    std::unique_ptr<TerrainOperation> streaming_;
    std::map<TerrainRegionKey, Region, TerrainRegionLess> regions_;
    std::set<TerrainRegionKey, TerrainRegionLess> dirty_;
    std::map<TerrainRegionKey, schema::TerrainRegionStateT, TerrainRegionLess> saved_regions_;
    std::string saved_slot_;
    TransformSettings active_pose_;
    bool generated_enabled_ = true;
    std::vector<Fragment> fragments_;
    std::vector<std::uint64_t> retired_ids_;
    std::uint64_t next_operation_ = 1;
    std::uint64_t next_pin_ = 1;
    std::uint64_t next_debris_ = 1;
    std::uint64_t revision_ = 0;
    std::uint64_t settings_revision_ = 1;
    std::uint64_t demand_revision_ = 1;
    std::uint64_t prepared_settings_revision_ = 0;
    bool plan_dirty_ = true;
    bool ready_ = false;
    std::string error_;
};
} // namespace engine::plugins::terrain
