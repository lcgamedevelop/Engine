#include "plugins/runtime/terrain/terrain_volume.hpp"

#include "plugins/runtime/terrain/terrain_samples.hpp"
#include "runtime/core/guid_map.hpp"
#include "runtime/performance/profiling.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <limits>
#include <set>
#include <utility>

namespace engine::plugins::terrain
{
namespace
{
using namespace terrain_detail;
constexpr std::size_t maximum_edit_regions = 256;
constexpr std::size_t maximum_brushes = 64;

bool block_valid(const schema::TerrainAssetDataT& source, const schema::TerrainBlockDataT& block)
{
    const auto count =
        static_cast<std::size_t>(source.block_cells) * source.block_cells * source.block_cells;
    const auto* region = entry(source, block.key);
    const auto samples = block.uniform ? std::size_t{1} : count;
    return block.schema_version == 2 && region &&
           region->uniform != schema::TerrainUniformState::unknown &&
           block.density.size() == samples && block.layers.size() == samples &&
           std::all_of(block.density.begin(), block.density.end(),
                       [](float v) { return std::isfinite(v); }) &&
           std::all_of(block.layers.begin(), block.layers.end(), [&](const auto& layer)
                       { return layer_valid(layer, source.material_layers.size()); });
}

Bounds3 brush_bounds(const schema::TerrainBrushDataT& brush)
{
    return {{brush.center.x() - brush.extent.x(), brush.center.y() - brush.extent.y(),
             brush.center.z() - brush.extent.z()},
            {brush.center.x() + brush.extent.x(), brush.center.y() + brush.extent.y(),
             brush.center.z() + brush.extent.z()}};
}

bool brush_valid(const schema::TerrainBrushDataT& brush, const schema::TerrainAssetDataT& source)
{
    return finite(brush.center) && finite(brush.extent) && brush.extent.x() > 0 &&
           brush.extent.y() > 0 && brush.extent.z() > 0 && std::isfinite(brush.strength) &&
           brush.strength > 0 && brush.strength <= 1 &&
           brush.shape >= schema::TerrainBrushShape::MIN &&
           brush.shape <= schema::TerrainBrushShape::MAX &&
           brush.operation >= schema::TerrainBrushOperation::MIN &&
           brush.operation <= schema::TerrainBrushOperation::MAX &&
           (brush.shape != schema::TerrainBrushShape::sphere ||
            (brush.extent.x() == brush.extent.y() && brush.extent.x() == brush.extent.z())) &&
           layer_valid(brush.layer, source.material_layers.size());
}

float signed_distance(const schema::TerrainBrushDataT& brush, Vec3 point)
{
    const double x = std::abs(point.x() - brush.center.x());
    const double y = std::abs(point.y() - brush.center.y());
    const double z = std::abs(point.z() - brush.center.z());
    if (brush.shape == schema::TerrainBrushShape::sphere)
    {
        return static_cast<float>(std::hypot(x, y, z) - brush.extent.x());
    }
    const double dx = x - brush.extent.x(), dy = y - brush.extent.y(), dz = z - brush.extent.z();
    return static_cast<float>(std::hypot(std::max(dx, 0.0), std::max(dy, 0.0), std::max(dz, 0.0)) +
                              std::min(std::max({dx, dy, dz}), 0.0));
}

using BrushFunction = float (*)(float value, float distance, double height, float smooth);
float dig(float value, float distance, double, float)
{
    return std::max(value, -distance);
}
float fill(float value, float distance, double, float)
{
    return std::min(value, distance);
}
float flatten(float value, float distance, double height, float)
{
    return distance <= 0 ? static_cast<float>(height) : value;
}
float smooth(float value, float distance, double, float average)
{
    return distance <= 0 ? average : value;
}
float paint(float value, float, double, float)
{
    return value;
}
constexpr std::array<BrushFunction, 5> brush_functions{dig, fill, flatten, smooth, paint};

} // namespace

bool TerrainVolume::validate(const schema::TerrainAssetDataT& source, std::string& error)
{
    error.clear();
    if (source.schema_version != 2 || !finite(source.origin) || !valid_bounds(source.bounds) ||
        !std::isfinite(source.voxel_size) || source.voxel_size < 0.01 || source.voxel_size > 1024 ||
        (source.block_cells != 8 && source.block_cells != 16 && source.block_cells != 32) ||
        source.regions.empty() || source.regions.size() > 4U * 1024U * 1024U ||
        source.material_layers.size() > 256 ||
        (!source.surface_material.empty() && !GuidMap::valid_guid(source.surface_material)))
    {
        error = "Invalid terrain index version, bounds, scale or capacity";
        return false;
    }
    const schema::TerrainRegionEntryT* previous = nullptr;
    for (const auto& region : source.regions)
    {
        if (!region || region->key.lod() > 15 ||
            (previous && !TerrainRegionLess{}(previous->key, region->key)) ||
            !intersects(source.bounds, region_bounds(source, region->key)) ||
            region->uniform < schema::TerrainUniformState::MIN ||
            region->uniform > schema::TerrainUniformState::MAX)
        {
            error = "Terrain regions must have unique sorted keys inside the declared bounds";
            return false;
        }
        if (region->uniform == schema::TerrainUniformState::mixed)
        {
            if (!GuidMap::valid_guid(region->payload) || region->content_hash.size() != 16 ||
                !std::all_of(region->content_hash.begin(), region->content_hash.end(), [](char c)
                             { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
            {
                error = "Mixed terrain regions require a payload GUID and catalog content hash";
                return false;
            }
        }
        else if (!region->payload.empty() || !region->content_hash.empty())
        {
            error = "Uniform or unknown terrain regions cannot reference a payload";
            return false;
        }
        previous = region.get();
    }
    for (const auto& material : source.material_layers)
    {
        if (!GuidMap::valid_guid(material))
        {
            error = "Invalid terrain material layer GUID";
            return false;
        }
    }
    return true;
}

bool TerrainVolume::initialize(const schema::TerrainAssetDataT& source, std::string& error)
{
    if (!validate(source, error))
    {
        return false;
    }
    auto candidate = std::make_shared<const schema::TerrainAssetDataT>(source);
    source_ = std::move(candidate);
    blocks_.clear();
    revision_ = 1;
    return true;
}

bool TerrainVolume::stage_regions(std::span<const schema::TerrainBlockDataT> blocks,
                                  std::span<const TerrainRegionKey> retire, TerrainDelta& output,
                                  std::string& error) const
{
    output = {};
    error.clear();
    if (!source_ || blocks.size() + retire.size() > maximum_edit_regions || revision_ == UINT64_MAX)
    {
        error = "Invalid terrain region batch or volume state";
        return false;
    }
    TerrainDelta candidate;
    candidate.source = source_;
    candidate.base_revision = revision_;
    candidate.blocks = blocks_;
    std::set<TerrainRegionKey, TerrainRegionLess> keys;
    for (auto key : retire)
    {
        if (!keys.insert(key).second || candidate.blocks.erase(key) == 0)
        {
            error = "Duplicate or nonresident terrain retirement";
            return false;
        }
        candidate.changed.push_back(key);
    }
    for (const auto& block : blocks)
    {
        if (!keys.insert(block.key).second || !block_valid(*source_, block))
        {
            error = "Invalid or duplicate terrain block payload";
            return false;
        }
        candidate.blocks[block.key] = std::make_shared<const schema::TerrainBlockDataT>(block);
        candidate.changed.push_back(block.key);
    }
    output = std::move(candidate);
    return true;
}

bool TerrainVolume::read(const TerrainRegionQuery& query, TerrainReadSnapshot& output) const
{
    output = {};
    TerrainReadSnapshot candidate;
    if (!source_ || !query.cursor.empty() || query.limit > 4096 ||
        !enumerate(*source_, query.bounds, query.lod, query.limit, candidate.regions))
    {
        return false;
    }
    candidate.source = source_;
    candidate.blocks = blocks_;
    candidate.source_revision = revision_;
    // The immutable snapshot includes the complete resident map; halo ownership is unchanged.
    for (auto key : candidate.regions)
    {
        for (int z = -1; z <= 1; ++z)
        {
            for (int y = -1; y <= 1; ++y)
            {
                for (int x = -1; x <= 1; ++x)
                {
                    if (key.x() <= INT32_MIN || key.x() >= INT32_MAX || key.y() <= INT32_MIN ||
                        key.y() >= INT32_MAX || key.z() <= INT32_MIN || key.z() >= INT32_MAX ||
                        !resident(candidate, {key.x() + x, key.y() + y, key.z() + z, key.lod()}))
                    {
                        return false;
                    }
                }
            }
        }
    }
    output = std::move(candidate);
    return true;
}

bool TerrainVolume::stage_edit(const TerrainReadSnapshot& input, const TerrainEditSpec& edit,
                               TerrainDelta& output, std::string& error)
{
    ENGINE_PROFILE_ZONE("Terrain.stage_edit");
    output = {};
    error.clear();
    if (!input.source || input.source_revision == 0 || input.source_revision == UINT64_MAX ||
        (edit.expected_revision && edit.expected_revision != input.source_revision) ||
        edit.brushes.empty() || edit.brushes.size() > maximum_brushes)
    {
        error = "Invalid terrain edit or revision conflict";
        return false;
    }
    TerrainDelta candidate;
    candidate.base_revision = input.source_revision;
    candidate.source = input.source;
    candidate.blocks = input.blocks;
    std::set<TerrainRegionKey, TerrainRegionLess> changed;
    std::array<double, 3> minimum{INFINITY, INFINITY, INFINITY};
    std::array<double, 3> maximum{-INFINITY, -INFINITY, -INFINITY};
    const auto cells = input.source->block_cells;
    for (const auto& brush : edit.brushes)
    {
        std::vector<TerrainRegionKey> keys;
        const auto bounds = brush_bounds(brush);
        if (!brush_valid(brush, *input.source) ||
            !enumerate(*input.source, bounds, 0, maximum_edit_regions, keys))
        {
            error = "Terrain brush is invalid or exceeds the bounded region batch";
            return false;
        }
        const std::array<double, 3> low{bounds.minimum().x(), bounds.minimum().y(),
                                        bounds.minimum().z()};
        const std::array<double, 3> high{bounds.maximum().x(), bounds.maximum().y(),
                                         bounds.maximum().z()};
        for (std::size_t axis = 0; axis < 3; ++axis)
        {
            minimum[axis] = std::min(minimum[axis], low[axis]);
            maximum[axis] = std::max(maximum[axis], high[axis]);
        }
        TerrainReadSnapshot current = input;
        current.blocks = candidate.blocks;
        for (auto key : keys)
        {
            if (std::find(input.regions.begin(), input.regions.end(), key) == input.regions.end())
            {
                error = "Terrain edit snapshot does not cover the complete brush";
                return false;
            }
            schema::TerrainBlockDataT block;
            if (!materialize_block(current, key, block))
            {
                error = "Terrain edit requires an unloaded or unknown region";
                return false;
            }
            bool modified = false;
            for (std::uint32_t z = 0; z < cells; ++z)
            {
                for (std::uint32_t y = 0; y < cells; ++y)
                {
                    for (std::uint32_t x = 0; x < cells; ++x)
                    {
                        const std::int64_t sx = static_cast<std::int64_t>(key.x()) * cells + x;
                        const std::int64_t sy = static_cast<std::int64_t>(key.y()) * cells + y;
                        const std::int64_t sz = static_cast<std::int64_t>(key.z()) * cells + z;
                        const auto index = (static_cast<std::size_t>(z) * cells + y) * cells + x;
                        const auto& source = *input.source;
                        const Vec3 position{source.origin.x() + sx * source.voxel_size,
                                            source.origin.y() + sy * source.voxel_size,
                                            source.origin.z() + sz * source.voxel_size};
                        const float distance = signed_distance(brush, position);
                        const float before = block.density[index];
                        float average = before;
                        if (brush.operation == schema::TerrainBrushOperation::smooth &&
                            distance <= 0)
                        {
                            std::array<float, 6> neighbors{};
                            if (!sample(current, sx - 1, sy, sz, 0, neighbors[0]) ||
                                !sample(current, sx + 1, sy, sz, 0, neighbors[1]) ||
                                !sample(current, sx, sy - 1, sz, 0, neighbors[2]) ||
                                !sample(current, sx, sy + 1, sz, 0, neighbors[3]) ||
                                !sample(current, sx, sy, sz - 1, 0, neighbors[4]) ||
                                !sample(current, sx, sy, sz + 1, 0, neighbors[5]))
                            {
                                error = "Terrain smoothing requires the complete sample halo";
                                return false;
                            }
                            average = 0;
                            for (float value : neighbors)
                            {
                                average += value / 6;
                            }
                        }
                        const float target =
                            brush_functions[static_cast<std::size_t>(brush.operation)](
                                before, distance, position.y() - brush.center.y(), average);
                        const float after =
                            std::clamp(before + (target - before) * brush.strength,
                                       -float(source.voxel_size), float(source.voxel_size));
                        if (after != before)
                        {
                            block.density[index] = after;
                            modified = true;
                        }
                        if (distance <= 0 &&
                            (brush.operation == schema::TerrainBrushOperation::paint ||
                             brush.operation == schema::TerrainBrushOperation::fill) &&
                            block.layers[index] != brush.layer)
                        {
                            block.layers[index] =
                                blend_layers(block.layers[index], brush.layer, brush.strength);
                            modified = true;
                        }
                    }
                }
            }
            if (modified)
            {
                candidate.blocks[key] =
                    std::make_shared<const schema::TerrainBlockDataT>(std::move(block));
                changed.insert(key);
                if (changed.size() > maximum_edit_regions)
                {
                    error = "Terrain brush batch exceeds the region limit";
                    return false;
                }
            }
        }
    }
    candidate.changed.assign(changed.begin(), changed.end());
    if (!propagate_lods(input, candidate, error))
    {
        return false;
    }
    candidate.dirty_bounds = {{minimum[0], minimum[1], minimum[2]},
                              {maximum[0], maximum[1], maximum[2]}};
    output = std::move(candidate);
    return true;
}

bool TerrainVolume::validate_delta(const TerrainDelta& delta) const noexcept
{
    return source_ && delta.source == source_ && delta.base_revision == revision_ &&
           revision_ != UINT64_MAX;
}

void TerrainVolume::publish(TerrainDelta&& delta) noexcept
{
    assert(validate_delta(delta));
    blocks_.swap(delta.blocks);
    ++revision_;
}
} // namespace engine::plugins::terrain
