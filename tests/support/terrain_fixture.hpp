#pragma once

#include "plugins/runtime/terrain/terrain_mesher.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace test_support
{
struct TerrainFixture
{
    engine::schema::TerrainAssetDataT source;
    std::vector<engine::schema::TerrainBlockDataT> blocks;

    TerrainFixture(engine::Bounds3 bounds, double voxel_size = 0.5, double height = 0)
    {
        source.bounds = bounds;
        source.block_cells = 8;
        source.voxel_size = voxel_size;
        const double span = voxel_size * 8;
        std::uint64_t identity = 1;
        for (int x = int(std::floor(bounds.minimum().x() / span));
             x < int(std::ceil(bounds.maximum().x() / span)); ++x)
        {
            for (int y = int(std::floor(bounds.minimum().y() / span));
                 y < int(std::ceil(bounds.maximum().y() / span)); ++y)
            {
                for (int z = int(std::floor(bounds.minimum().z() / span));
                     z < int(std::ceil(bounds.maximum().z() / span)); ++z)
                {
                    auto entry = std::make_unique<engine::schema::TerrainRegionEntryT>();
                    entry->key = {x, y, z, 0};
                    entry->uniform = engine::schema::TerrainUniformState::mixed;
                    char guid[37];
                    std::snprintf(guid, sizeof(guid), "12121212-3434-4567-8910-%012llx",
                                  static_cast<unsigned long long>(identity++));
                    entry->payload = guid;
                    entry->content_hash.assign(16, 'a');
                    engine::schema::TerrainBlockDataT block;
                    block.key = entry->key;
                    for (int pz = 0; pz < 8; ++pz)
                    {
                        for (int py = 0; py < 8; ++py)
                        {
                            for (int px = 0; px < 8; ++px)
                            {
                                block.density.push_back(float(std::clamp(
                                    (y * 8 + py) * voxel_size - height, -voxel_size, voxel_size)));
                                block.layers.emplace_back(0, 0, 0, 0, engine::Vec4{1, 0, 0, 0});
                            }
                        }
                    }
                    source.regions.push_back(std::move(entry));
                    blocks.push_back(std::move(block));
                }
            }
        }
    }

    bool restore(engine::plugins::terrain::TerrainVolume& volume, std::string& error) const
    {
        engine::plugins::terrain::TerrainDelta delta;
        if (!volume.initialize(source, error) || !volume.stage_regions(blocks, {}, delta, error))
        {
            return false;
        }
        volume.publish(std::move(delta));
        return true;
    }
};

// Navigation consumes world-space triangles assembled from the real region mesher.
inline bool terrain_navigation_mesh(const engine::plugins::terrain::TerrainVolume& volume,
                                    const engine::Bounds3& bounds,
                                    engine::schema::MeshDataT& output, std::string& error)
{
    using namespace engine::plugins::terrain;
    output = {};
    TerrainReadSnapshot snapshot;
    TerrainRegionQuery query;
    query.bounds = bounds;
    auto mesher = create_terrain_mesher(error);
    if (!mesher || !volume.read(query, snapshot))
    {
        return false;
    }
    for (auto key : snapshot.regions)
    {
        TerrainMeshInput input;
        input.samples = snapshot;
        input.key = key;
        TerrainMeshOutput mesh;
        if (!mesher->build(input, mesh, error))
        {
            return false;
        }
        if (!mesh.mesh)
        {
            continue;
        }
        const auto base = static_cast<std::uint32_t>(output.vertices.size());
        for (auto vertex : mesh.mesh->vertices)
        {
            vertex.mutate_x(vertex.x() + float(mesh.local_bounds.minimum().x()));
            vertex.mutate_y(vertex.y() + float(mesh.local_bounds.minimum().y()));
            vertex.mutate_z(vertex.z() + float(mesh.local_bounds.minimum().z()));
            output.vertices.push_back(vertex);
        }
        for (auto index : mesh.mesh->indices)
        {
            output.indices.push_back(base + index);
        }
    }
    return true;
}
} // namespace test_support
