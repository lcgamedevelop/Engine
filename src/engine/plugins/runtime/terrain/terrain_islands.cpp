#include "plugins/runtime/terrain/terrain_islands.hpp"
#include "plugins/runtime/terrain/terrain_samples.hpp"
#include "runtime/performance/profiling.hpp"

// Six-neighbor island identification follows Godot Voxel Tools (MIT), commit
// 045ff9326a32fd10ac6fdac6357b5a098aaf2093, util/island_finder.h.
// Ownership, bounded sparse queries and publication use this engine's own contracts.
#include <algorithm>
#include <array>
#include <map>
#include <set>

namespace engine::plugins::terrain
{
namespace
{
using namespace terrain_detail;
using Point = std::array<std::int64_t, 3>;
using Keys = std::set<TerrainRegionKey, TerrainRegionLess>;
std::int64_t floor_div(std::int64_t value, std::int64_t divisor)
{
    return value / divisor - (value % divisor < 0 ? 1 : 0);
}
TerrainRegionKey owner(Point point, std::uint32_t cells)
{
    return {std::int32_t(floor_div(point[0], cells)), std::int32_t(floor_div(point[1], cells)),
            std::int32_t(floor_div(point[2], cells)), 0};
}
Vec3 position(const schema::TerrainAssetDataT& source, Point point)
{
    return {source.origin.x() + point[0] * source.voxel_size,
            source.origin.y() + point[1] * source.voxel_size,
            source.origin.z() + point[2] * source.voxel_size};
}
std::size_t offset(Point point, std::uint32_t cells)
{
    const auto local = [cells](std::int64_t v)
    { return std::size_t(v - floor_div(v, cells) * cells); };
    return (local(point[2]) * cells + local(point[1])) * cells + local(point[0]);
}
bool build_fragment(const TerrainReadSnapshot& isolated, const TerrainMesher& mesher,
                    TerrainFragment& output, std::string& error)
{
    output.samples = isolated;
    output.bounds = isolated.source->bounds;
    output.position = output.bounds.minimum();
    const auto& origin = output.position;
    for (auto key : isolated.regions)
    {
        TerrainMeshInput input;
        input.samples = isolated;
        input.key = key;
        TerrainMeshOutput mesh;
        if (!mesher.build(input, mesh, error))
        {
            return false;
        }
        if (!mesh.mesh)
        {
            continue;
        }
        const auto base = std::uint32_t(output.mesh.vertices.size());
        const auto b = mesh.local_bounds.minimum();
        for (const auto& v : mesh.mesh->vertices)
        {
            output.mesh.vertices.emplace_back(
                float(v.x() + b.x() - origin.x()), float(v.y() + b.y() - origin.y()),
                float(v.z() + b.z() - origin.z()), v.nx(), v.ny(), v.nz());
        }
        for (auto index : mesh.mesh->indices)
        {
            output.mesh.indices.push_back(base + index);
        }
        output.mesh.colors.insert(output.mesh.colors.end(), mesh.mesh->colors.begin(),
                                  mesh.mesh->colors.end());
        output.mesh.texcoords.insert(output.mesh.texcoords.end(), mesh.mesh->texcoords.begin(),
                                     mesh.mesh->texcoords.end());
    }
    const auto& source = *isolated.source;
    const auto n = source.block_cells;
    std::set<Point> cells;
    for (const auto& [key, block] : isolated.blocks)
    {
        for (std::uint32_t z = 0; z < n; ++z)
        {
            for (std::uint32_t y = 0; y < n; ++y)
            {
                for (std::uint32_t x = 0; x < n; ++x)
                {
                    const auto i = (std::size_t(z) * n + y) * n + x;
                    if (block->density[i] >= 0)
                    {
                        continue;
                    }
                    output.volume += source.voxel_size * source.voxel_size * source.voxel_size;
                    const Point sample{std::int64_t(key.x()) * n + x, std::int64_t(key.y()) * n + y,
                                       std::int64_t(key.z()) * n + z};
                    for (int dz = -1; dz <= 0; ++dz)
                    {
                        for (int dy = -1; dy <= 0; ++dy)
                        {
                            for (int dx = -1; dx <= 0; ++dx)
                            {
                                cells.insert({sample[0] + dx, sample[1] + dy, sample[2] + dz});
                            }
                        }
                    }
                }
            }
        }
    }
    if (cells.size() > 4096)
    {
        error = "Terrain fragment exceeds the convex-cell budget";
        return false;
    }
    output.collision.shape = schema::ColliderShape::compound;
    for (const auto& cell : cells)
    {
        std::array<float, 8> densities{};
        std::array<Vec3, 8> points{};
        auto hull = std::make_unique<schema::ColliderHullDataT>();
        for (unsigned c = 0; c < 8; ++c)
        {
            const Point p{cell[0] + (c & 1), cell[1] + ((c >> 1) & 1), cell[2] + ((c >> 2) & 1)};
            if (!sample(isolated, p[0], p[1], p[2], 0, densities[c]))
            {
                error = "Incomplete fragment sample halo";
                return false;
            }
            const auto value = position(source, p);
            points[c] = {value.x() - origin.x(), value.y() - origin.y(), value.z() - origin.z()};
            if (densities[c] < 0)
            {
                hull->vertices.push_back(points[c]);
            }
        }
        for (unsigned c = 0; c < 8; ++c)
        {
            for (unsigned bit = 1; bit <= 4; bit *= 2)
            {
                if ((c & bit) || (densities[c] < 0) == (densities[c | bit] < 0))
                {
                    continue;
                }
                const auto other = c | bit;
                const double t = densities[c] / double(densities[c] - densities[other]);
                const auto a = points[c], b = points[other];
                hull->vertices.emplace_back(a.x() + t * (b.x() - a.x()),
                                            a.y() + t * (b.y() - a.y()),
                                            a.z() + t * (b.z() - a.z()));
            }
        }
        if (hull->vertices.size() >= 4)
        {
            output.collision.hulls.push_back(std::move(hull));
        }
    }
    if (output.mesh.indices.empty() || output.collision.hulls.empty())
    {
        error = "Terrain fragment has no closed geometry";
        return false;
    }
    return true;
}

bool isolate(const TerrainReadSnapshot& input, std::span<const Point> points,
             TerrainReadSnapshot& output, std::string& error)
{
    auto source = std::make_shared<schema::TerrainAssetDataT>();
    source->origin = input.source->origin;
    source->voxel_size = input.source->voxel_size;
    source->block_cells = input.source->block_cells;
    source->material_layers = input.source->material_layers;
    source->surface_material = input.source->surface_material;
    const auto n = source->block_cells;
    Keys keys;
    for (const auto& point : points)
    {
        const auto key = owner(point, n);
        for (int z = -1; z <= 1; ++z)
        {
            for (int y = -1; y <= 1; ++y)
            {
                for (int x = -1; x <= 1; ++x)
                {
                    keys.emplace(key.x() + x, key.y() + y, key.z() + z, 0);
                }
            }
        }
    }
    Vec3 low{INFINITY, INFINITY, INFINITY}, high{-INFINITY, -INFINITY, -INFINITY};
    for (auto key : keys)
    {
        const auto b = region_bounds(*source, key);
        low = {std::min(low.x(), b.minimum().x()), std::min(low.y(), b.minimum().y()),
               std::min(low.z(), b.minimum().z())};
        high = {std::max(high.x(), b.maximum().x()), std::max(high.y(), b.maximum().y()),
                std::max(high.z(), b.maximum().z())};
        auto entry = std::make_unique<schema::TerrainRegionEntryT>();
        entry->key = key;
        entry->uniform = schema::TerrainUniformState::air;
        source->regions.push_back(std::move(entry));
    }
    source->bounds = {low, high};
    output.source = source;
    output.source_revision = input.source_revision;
    std::map<TerrainRegionKey, std::shared_ptr<schema::TerrainBlockDataT>, TerrainRegionLess>
        blocks;
    for (const auto& point : points)
    {
        const auto key = owner(point, n);
        auto& block = blocks[key];
        if (!block)
        {
            block = std::make_shared<schema::TerrainBlockDataT>();
            block->key = key;
            block->density.assign(std::size_t(n) * n * n, float(source->voxel_size));
            block->layers.assign(block->density.size(),
                                 schema::TerrainLayerSample{0, 0, 0, 0, Vec4{1, 0, 0, 0}});
        }
        const auto i = offset(point, n);
        if (!sample(input, point[0], point[1], point[2], 0, block->density[i], &block->layers[i]))
        {
            error = "Missing isolated sample";
            return false;
        }
    }
    for (auto& [key, block] : blocks)
    {
        output.blocks.emplace(key, std::move(block));
    }
    output.regions.assign(keys.begin(), keys.end());
    return true;
}
} // namespace

bool stage_collapse(const TerrainReadSnapshot& input, const TerrainCollapseSpec& spec,
                    const TerrainMesher& mesher, TerrainDelta& output,
                    std::vector<TerrainFragment>& fragments,
                    const std::shared_ptr<std::atomic<bool>>& cancelled, std::string& error)
{
    ENGINE_PROFILE_ZONE("Terrain.collapse");
    output = {};
    fragments.clear();
    error.clear();
    if (!input.source || input.regions.empty() ||
        input.regions.size() > spec.connectivity_block_limit)
    {
        error = "Collapse connectivity exceeds the region budget";
        return false;
    }
    const auto& source = *input.source;
    const auto n = source.block_cells;
    std::set<Point> visited;
    std::map<TerrainRegionKey, std::shared_ptr<schema::TerrainBlockDataT>, TerrainRegionLess>
        changed;
    TerrainDelta candidate{input.source_revision, spec.bounds, input.source, input.blocks, {}};
    std::vector<TerrainFragment> pieces;
    const std::size_t limit = std::size_t(spec.connectivity_block_limit) * n * n * n;
    Keys traversed;
    for (auto key : input.regions)
    {
        for (std::uint32_t z = 0; z < n; ++z)
        {
            for (std::uint32_t y = 0; y < n; ++y)
            {
                for (std::uint32_t x = 0; x < n; ++x)
                {
                    if (cancelled && cancelled->load(std::memory_order_relaxed))
                    {
                        error = "Terrain collapse cancelled";
                        return false;
                    }
                    const Point seed{std::int64_t(key.x()) * n + x, std::int64_t(key.y()) * n + y,
                                     std::int64_t(key.z()) * n + z};
                    if (visited.contains(seed))
                    {
                        continue;
                    }
                    float value = 0;
                    if (!sample(input, seed[0], seed[1], seed[2], 0, value))
                    {
                        error = "Collapse needs an unloaded sample";
                        return false;
                    }
                    if (value >= 0)
                    {
                        continue;
                    }
                    std::vector<Point> island{seed};
                    visited.insert(seed);
                    bool anchored = false, incomplete = false;
                    for (std::size_t cursor = 0; cursor < island.size(); ++cursor)
                    {
                        const auto p = island[cursor];
                        const auto world = position(source, p);
                        const double epsilon = source.voxel_size;
                        anchored |= world.y() <= source.bounds.minimum().y() + epsilon ||
                                    world.x() <= source.bounds.minimum().x() + epsilon ||
                                    world.x() >= source.bounds.maximum().x() - epsilon ||
                                    world.z() <= source.bounds.minimum().z() + epsilon ||
                                    world.z() >= source.bounds.maximum().z() - epsilon;
                        traversed.insert(owner(p, n));
                        if (visited.size() > limit ||
                            traversed.size() > spec.connectivity_block_limit)
                        {
                            error =
                                "Collapse connectivity budget exhausted; no terrain was changed";
                            return false;
                        }
                        for (int axis = 0; axis < 3; ++axis)
                        {
                            for (int step : {-1, 1})
                            {
                                auto neighbor = p;
                                neighbor[axis] += step;
                                if (visited.contains(neighbor))
                                {
                                    continue;
                                }
                                float density = 0;
                                if (!sample(input, neighbor[0], neighbor[1], neighbor[2], 0,
                                            density))
                                {
                                    incomplete = true;
                                    continue;
                                }
                                if (density < 0)
                                {
                                    visited.insert(neighbor);
                                    island.push_back(neighbor);
                                }
                            }
                        }
                    }
                    if (anchored)
                    {
                        continue;
                    }
                    if (incomplete)
                    {
                        error = "Collapse connectivity is incomplete; load and pin the neighboring "
                                "regions";
                        return false;
                    }
                    if (island.size() < spec.min_samples)
                    {
                        continue;
                    }
                    std::map<Point, std::vector<Point>> partitions;
                    for (const auto& p : island)
                    {
                        partitions[{floor_div(p[0], spec.fragment_cells),
                                    floor_div(p[1], spec.fragment_cells),
                                    floor_div(p[2], spec.fragment_cells)}]
                            .push_back(p);
                    }
                    if (pieces.size() + partitions.size() > spec.max_fragments)
                    {
                        error = "Collapse exceeds the fragment budget";
                        return false;
                    }
                    for (auto& [partition, points] : partitions)
                    {
                        (void)partition;
                        TerrainReadSnapshot isolated;
                        TerrainFragment piece;
                        if (!isolate(input, points, isolated, error) ||
                            !build_fragment(isolated, mesher, piece, error))
                        {
                            return false;
                        }
                        Keys origins;
                        for (const auto& p : points)
                        {
                            const auto region = owner(p, n);
                            origins.insert(region);
                            auto& block = changed[region];
                            if (!block)
                            {
                                block = std::make_shared<schema::TerrainBlockDataT>();
                                if (!materialize_block(input, region, *block))
                                {
                                    error = "Missing collapse source";
                                    return false;
                                }
                            }
                            block->density[offset(p, n)] = std::max(
                                float(source.voxel_size), std::abs(block->density[offset(p, n)]));
                        }
                        piece.source_regions.assign(origins.begin(), origins.end());
                        piece.source_region = *origins.begin();
                        pieces.push_back(std::move(piece));
                    }
                }
            }
        }
    }
    for (auto& [key, block] : changed)
    {
        candidate.blocks[key] = std::move(block);
        candidate.changed.push_back(key);
    }
    if (!propagate_lods(input, candidate, error))
    {
        return false;
    }
    output = std::move(candidate);
    fragments = std::move(pieces);
    return true;
}

bool rebuild_fragment(const TerrainReadSnapshot& input, const TerrainMesher& mesher,
                      TerrainFragment& output, std::string& error)
{
    return build_fragment(input, mesher, output, error);
}
} // namespace engine::plugins::terrain
