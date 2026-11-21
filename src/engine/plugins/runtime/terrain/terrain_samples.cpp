#include "plugins/runtime/terrain/terrain_samples.hpp"
#include "runtime/performance/function_profile.hpp"
#include "runtime/performance/profiling.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <tuple>

namespace engine::plugins::terrain
{
bool TerrainRegionLess::operator()(const TerrainRegionKey& a,
                                   const TerrainRegionKey& b) const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return std::tuple{a.lod(), a.x(), a.y(), a.z()} < std::tuple{b.lod(), b.x(), b.y(), b.z()};
}
namespace terrain_detail
{
schema::TerrainLayerSample blend_layers(const schema::TerrainLayerSample& a,
                                        const schema::TerrainLayerSample& b, double weight)
{
    ENGINE_PROFILE_FUNCTION();
    std::map<std::uint32_t, double> values;
    for (const auto& [sample, scale] : {std::pair{a, 1 - weight}, std::pair{b, weight}})
    {
        const std::array ids{sample.layer0(), sample.layer1(), sample.layer2(), sample.layer3()};
        const auto w = sample.weights();
        const std::array weights{w.x(), w.y(), w.z(), w.w()};
        for (unsigned i = 0; i < 4; ++i)
        {
            values[ids[i]] += weights[i] * scale;
        }
    }
    std::vector<std::pair<std::uint32_t, double>> sorted(values.begin(), values.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b)
              {
        ENGINE_PROFILE_FUNCTION_PROBE();
        return a.second != b.second ? a.second > b.second : a.first < b.first;
    });
    sorted.resize(4);
    const auto sum = sorted[0].second + sorted[1].second + sorted[2].second + sorted[3].second;
    return {sorted[0].first,
            sorted[1].first,
            sorted[2].first,
            sorted[3].first,
            {sorted[0].second / sum, sorted[1].second / sum, sorted[2].second / sum,
             sorted[3].second / sum}};
}
bool finite(const Vec3& v) noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return std::isfinite(v.x()) && std::isfinite(v.y()) && std::isfinite(v.z());
}
bool valid_bounds(const Bounds3& b) noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return finite(b.minimum()) && finite(b.maximum()) && b.minimum().x() < b.maximum().x() &&
           b.minimum().y() < b.maximum().y() && b.minimum().z() < b.maximum().z();
}
bool intersects(const Bounds3& a, const Bounds3& b) noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return a.minimum().x() < b.maximum().x() && b.minimum().x() < a.maximum().x() &&
           a.minimum().y() < b.maximum().y() && b.minimum().y() < a.maximum().y() &&
           a.minimum().z() < b.maximum().z() && b.minimum().z() < a.maximum().z();
}
Bounds3 region_bounds(const schema::TerrainAssetDataT& source, TerrainRegionKey key) noexcept
{
    ENGINE_PROFILE_FUNCTION();
    const auto size = std::ldexp(source.voxel_size * source.block_cells, key.lod());
    const Vec3 minimum{source.origin.x() + key.x() * size, source.origin.y() + key.y() * size,
                       source.origin.z() + key.z() * size};
    return {minimum, {minimum.x() + size, minimum.y() + size, minimum.z() + size}};
}
const schema::TerrainRegionEntryT* entry(const schema::TerrainAssetDataT& source,
                                         TerrainRegionKey key) noexcept
{
    ENGINE_PROFILE_FUNCTION();
    const auto found = std::lower_bound(source.regions.begin(), source.regions.end(), key,
                                        [](const auto& value, const auto& key)
                                        {
                                            ENGINE_PROFILE_FUNCTION_PROBE();
                                            return TerrainRegionLess{}(value->key, key);
                                        });
    return found != source.regions.end() && (*found)->key == key ? found->get() : nullptr;
}
bool enumerate(const schema::TerrainAssetDataT& source, const Bounds3& bounds, std::uint8_t lod,
               std::size_t limit, std::vector<TerrainRegionKey>& output)
{
    ENGINE_PROFILE_FUNCTION();
    output.clear();
    if (!valid_bounds(bounds) || lod > 15 || limit == 0 || limit > 65536)
    {
        return false;
    }
    const double size = std::ldexp(source.voxel_size * source.block_cells, lod);
    const std::array<double, 3> origin{source.origin.x(), source.origin.y(), source.origin.z()};
    const std::array<double, 3> low{std::max(bounds.minimum().x(), source.bounds.minimum().x()),
                                    std::max(bounds.minimum().y(), source.bounds.minimum().y()),
                                    std::max(bounds.minimum().z(), source.bounds.minimum().z())};
    const std::array<double, 3> high{std::min(bounds.maximum().x(), source.bounds.maximum().x()),
                                     std::min(bounds.maximum().y(), source.bounds.maximum().y()),
                                     std::min(bounds.maximum().z(), source.bounds.maximum().z())};
    std::array<std::int64_t, 3> first{}, end{};
    std::size_t count = 1;
    for (std::size_t axis = 0; axis < 3; ++axis)
    {
        if (low[axis] >= high[axis])
        {
            return true;
        }
        const double minimum = std::floor((low[axis] - origin[axis]) / size);
        const double maximum = std::ceil((high[axis] - origin[axis]) / size);
        if (minimum < INT32_MIN || maximum > INT32_MAX || maximum - minimum > limit)
        {
            return false;
        }
        first[axis] = static_cast<std::int64_t>(minimum);
        end[axis] = static_cast<std::int64_t>(maximum);
        const auto width = static_cast<std::size_t>(end[axis] - first[axis]);
        if (width == 0 || count > limit / width)
        {
            return false;
        }
        count *= width;
    }
    output.reserve(count);
    for (auto x = first[0]; x < end[0]; ++x)
    {
        for (auto y = first[1]; y < end[1]; ++y)
        {
            for (auto z = first[2]; z < end[2]; ++z)
            {
                output.emplace_back(static_cast<std::int32_t>(x), static_cast<std::int32_t>(y),
                                    static_cast<std::int32_t>(z), lod);
            }
        }
    }
    return true;
}
bool layer_valid(const schema::TerrainLayerSample& value, std::size_t layer_count) noexcept
{
    ENGINE_PROFILE_FUNCTION();
    const std::array<std::uint32_t, 4> ids{value.layer0(), value.layer1(), value.layer2(),
                                           value.layer3()};
    const auto& w = value.weights();
    const std::array<double, 4> weights{w.x(), w.y(), w.z(), w.w()};
    double total = 0;
    for (std::size_t i = 0; i < weights.size(); ++i)
    {
        if (!std::isfinite(weights[i]) || weights[i] < 0 || weights[i] > 1 ||
            (weights[i] > 0 && ids[i] >= std::max<std::size_t>(layer_count, 1)))
        {
            return false;
        }
        total += weights[i];
    }
    return std::abs(total - 1.0) < 1e-5;
}
bool resident(const TerrainReadSnapshot& input, TerrainRegionKey key) noexcept
{
    ENGINE_PROFILE_FUNCTION();
    if (!input.source)
    {
        return false;
    }
    if (!intersects(input.source->bounds, region_bounds(*input.source, key)))
    {
        return true;
    }
    if (input.blocks.contains(key))
    {
        return true;
    }
    const auto* region = entry(*input.source, key);
    return region && (region->uniform == schema::TerrainUniformState::air ||
                      region->uniform == schema::TerrainUniformState::solid);
}
bool sample(const TerrainReadSnapshot& input, std::int64_t x, std::int64_t y, std::int64_t z,
            std::uint8_t lod, float& density, schema::TerrainLayerSample* layer) noexcept
{
    ENGINE_PROFILE_FUNCTION();
    density = 0;
    if (layer)
    {
        *layer = {0, 0, 0, 0, {1, 0, 0, 0}};
    }
    if (!input.source || lod > 15)
    {
        return false;
    }
    const auto& source = *input.source;
    const auto cells = static_cast<std::int64_t>(source.block_cells);
    const auto divide = [cells](std::int64_t value)
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        return value / cells - (value % cells < 0 ? 1 : 0);
    };
    const std::array<std::int64_t, 3> block{divide(x), divide(y), divide(z)};
    if (std::any_of(block.begin(), block.end(),
                    [](auto v)
                    {
                        ENGINE_PROFILE_FUNCTION_PROBE();
                        return v < INT32_MIN || v > INT32_MAX;
                    }))
    {
        return false;
    }
    const TerrainRegionKey key{static_cast<std::int32_t>(block[0]),
                               static_cast<std::int32_t>(block[1]),
                               static_cast<std::int32_t>(block[2]), lod};
    const double spacing = std::ldexp(source.voxel_size, lod);
    const auto& bounds = source.bounds;
    const Vec3 position{source.origin.x() + x * spacing, source.origin.y() + y * spacing,
                        source.origin.z() + z * spacing};
    if (position.x() < bounds.minimum().x() || position.x() >= bounds.maximum().x() ||
        position.y() < bounds.minimum().y() || position.y() >= bounds.maximum().y() ||
        position.z() < bounds.minimum().z() || position.z() >= bounds.maximum().z())
    {
        density = static_cast<float>(spacing);
        return true;
    }
    const auto found = input.blocks.find(key);
    if (found != input.blocks.end())
    {
        const auto offset =
            found->second->uniform
                ? std::size_t{0}
                : static_cast<std::size_t>(((z - block[2] * cells) * cells + y - block[1] * cells) *
                                               cells +
                                           x - block[0] * cells);
        density = found->second->density[offset];
        if (layer)
        {
            *layer = found->second->layers[offset];
        }
        return true;
    }
    const auto* region = entry(source, key);
    if (!region || (region->uniform != schema::TerrainUniformState::air &&
                    region->uniform != schema::TerrainUniformState::solid))
    {
        return false;
    }
    density = static_cast<float>(region->uniform == schema::TerrainUniformState::solid
                                     ? -source.voxel_size
                                     : source.voxel_size);
    return true;
}
bool materialize_block(const TerrainReadSnapshot& input, TerrainRegionKey key,
                       schema::TerrainBlockDataT& output)
{
    ENGINE_PROFILE_FUNCTION();
    output = {};
    output.key = key;
    const auto cells = input.source->block_cells;
    const auto count = static_cast<std::size_t>(cells) * cells * cells;
    output.density.resize(count);
    output.layers.resize(count);
    for (std::uint32_t z = 0; z < cells; ++z)
    {
        for (std::uint32_t y = 0; y < cells; ++y)
        {
            for (std::uint32_t x = 0; x < cells; ++x)
            {
                const auto offset = (static_cast<std::size_t>(z) * cells + y) * cells + x;
                if (!sample(input, static_cast<std::int64_t>(key.x()) * cells + x,
                            static_cast<std::int64_t>(key.y()) * cells + y,
                            static_cast<std::int64_t>(key.z()) * cells + z, key.lod(),
                            output.density[offset], &output.layers[offset]))
                {
                    output = {};
                    return false;
                }
            }
        }
    }
    return true;
}

bool propagate_lods(const TerrainReadSnapshot& input, TerrainDelta& candidate, std::string& error)
{
    ENGINE_PROFILE_FUNCTION();
    const auto cells = input.source->block_cells;
    std::set<TerrainRegionKey, TerrainRegionLess> changed;
    for (auto key : candidate.changed)
    {
        if (key.lod() == 0)
        {
            changed.insert(key);
        }
    }
    candidate.changed.clear();
    std::set<TerrainRegionKey, TerrainRegionLess> coarse_regions;
    for (auto key : changed)
    {
        for (std::uint8_t lod = 1; lod <= 15; ++lod)
        {
            const std::int64_t divisor = std::int64_t{1} << lod;
            const auto parent = [divisor](std::int32_t coordinate)
            {
                ENGINE_PROFILE_FUNCTION_PROBE();
                return static_cast<std::int32_t>(coordinate / divisor -
                                                 (coordinate % divisor < 0 ? 1 : 0));
            };
            TerrainRegionKey coarse{parent(key.x()), parent(key.y()), parent(key.z()), lod};
            if (entry(*input.source, coarse))
            {
                coarse_regions.insert(coarse);
            }
        }
    }
    TerrainReadSnapshot fine = input;
    fine.blocks = candidate.blocks;
    for (auto key : coarse_regions)
    {
        schema::TerrainBlockDataT block;
        if (!materialize_block(fine, key, block))
        {
            error = "Terrain edit requires its authored coarse-level regions";
            return false;
        }
        const std::int64_t scale = std::int64_t{1} << key.lod();
        bool modified = false;
        for (std::uint32_t z = 0; z < cells; ++z)
        {
            for (std::uint32_t y = 0; y < cells; ++y)
            {
                for (std::uint32_t x = 0; x < cells; ++x)
                {
                    const std::int64_t sx = (std::int64_t(key.x()) * cells + x) * scale;
                    const std::int64_t sy = (std::int64_t(key.y()) * cells + y) * scale;
                    const std::int64_t sz = (std::int64_t(key.z()) * cells + z) * scale;
                    const auto owner = [cells](std::int64_t coordinate)
                    {
                        ENGINE_PROFILE_FUNCTION_PROBE();
                        return static_cast<std::int32_t>(coordinate / cells -
                                                         (coordinate % cells < 0 ? 1 : 0));
                    };
                    if (!changed.contains({owner(sx), owner(sy), owner(sz), 0}))
                    {
                        continue;
                    }
                    const auto offset = (static_cast<std::size_t>(z) * cells + y) * cells + x;
                    if (!sample(fine, sx, sy, sz, 0, block.density[offset], &block.layers[offset]))
                    {
                        error = "Terrain coarse update requires its authoritative samples";
                        return false;
                    }
                    modified = true;
                }
            }
        }
        if (modified)
        {
            candidate.blocks[key] =
                std::make_shared<const schema::TerrainBlockDataT>(std::move(block));
            candidate.changed.push_back(key);
        }
    }
    candidate.changed.insert(candidate.changed.end(), changed.begin(), changed.end());
    std::sort(candidate.changed.begin(), candidate.changed.end(), TerrainRegionLess{});
    if (candidate.changed.size() > 256)
    {
        error = "Terrain edit and its LOD neighborhood exceed the region limit";
        return false;
    }
    return true;
}
} // namespace terrain_detail
} // namespace engine::plugins::terrain
