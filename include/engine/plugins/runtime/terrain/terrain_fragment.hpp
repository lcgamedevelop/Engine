#pragma once

#include "plugins/runtime/terrain/terrain_volume.hpp"
#include "runtime/schema/mesh_generated.h"
#include "runtime/schema/scene_generated.h"

namespace engine::plugins::terrain
{

// Candidate-only geometry; not a save format. The runtime owns the resulting scene nodes.
struct TerrainFragment
{
    schema::MeshDataT mesh;
    schema::ColliderDataT collision;
    double volume = 0;
    float density = 1600;
    std::uint64_t stable_id = 0;
    TerrainRegionKey source_region;
    std::vector<TerrainRegionKey> source_regions;
    Bounds3 bounds;
    TerrainReadSnapshot samples;
    Vec3 position;
    Vec3 rotation;
    Vec3 linear_velocity;
    Vec3 angular_velocity;
    double age = 0;
    bool from_checkpoint = false;
    bool sleeping = false;
};

} // namespace engine::plugins::terrain
