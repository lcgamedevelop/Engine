#pragma once

#include "plugins/runtime/terrain/terrain_volume.hpp"
#include "runtime/physics/mesh_collision_data.hpp"
#include "runtime/schema/mesh_generated.h"

#include <atomic>
#include <memory>
#include <string>

namespace engine::plugins::terrain
{
struct TerrainMeshInput
{
    TerrainReadSnapshot samples;
    TerrainRegionKey key;
    // Fine-to-coarse faces: -X, +X, -Y, +Y, -Z, +Z. Adjacent LOD differs by one.
    std::uint8_t transition_faces = 0;
    bool collision_required = false;
    std::shared_ptr<const std::atomic<bool>> cancel_requested;
};

struct TerrainMeshOutput
{
    TerrainRegionKey key;
    Bounds3 local_bounds;
    std::uint64_t source_revision = 0;
    std::shared_ptr<const schema::MeshDataT> mesh;
    MeshCollisionData collision;
};

class TerrainMesher
{
public:
    virtual ~TerrainMesher() noexcept = default;
    [[nodiscard]] virtual bool build(const TerrainMeshInput& input, TerrainMeshOutput& output,
                                     std::string& error) const = 0;
};

[[nodiscard]] std::unique_ptr<TerrainMesher> create_terrain_mesher(std::string& error);
} // namespace engine::plugins::terrain
