#include "plugins/editor/terrain/terrain_generator.hpp"
#include "plugins/runtime/terrain/terrain_samples.hpp"
#include <FastNoise/FastNoise.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <tuple>

namespace engine::plugins::terrain
{
namespace
{
double base_height(const schema::TerrainGenerationSpecT& spec, const FastNoise::Generator& noise,
                   double x, double z)
{
    double height = spec.height;
    if (spec.heightmap)
    {
        const auto& map = *spec.heightmap;
        const auto& low = spec.bounds.minimum();
        const auto& high = spec.bounds.maximum();
        const double u =
            std::clamp((x - low.x()) / (high.x() - low.x()), 0.0, 1.0) * (map.width - 1);
        const double v =
            std::clamp((z - low.z()) / (high.z() - low.z()), 0.0, 1.0) * (map.height - 1);
        const auto ix = std::min(std::uint32_t(u), map.width - 2);
        const auto iz = std::min(std::uint32_t(v), map.height - 2);
        const auto sample = [&](unsigned dx, unsigned dz)
        { return double(map.samples[(iz + dz) * map.width + ix + dx]); };
        const auto a = std::lerp(sample(0, 0), sample(1, 0), u - ix);
        const auto b = std::lerp(sample(0, 1), sample(1, 1), u - ix);
        height += map.height_offset + std::lerp(a, b, v - iz) * map.height_scale;
    }
    const double value = noise.GenSingle2D(float(x * spec.frequency), float(z * spec.frequency),
                                           std::bit_cast<std::int32_t>(spec.seed));
    return height + spec.amplitude * std::clamp(value, -1.0, 1.0);
}
double stamp_distance(const schema::TerrainBrushDataT& stamp, Vec3 p)
{
    const double x = std::abs(p.x() - stamp.center.x()), y = std::abs(p.y() - stamp.center.y()),
                 z = std::abs(p.z() - stamp.center.z());
    if (stamp.shape == schema::TerrainBrushShape::sphere)
    {
        return std::hypot(x, y, z) - stamp.extent.x();
    }
    const double dx = x - stamp.extent.x(), dy = y - stamp.extent.y(), dz = z - stamp.extent.z();
    return std::hypot(std::max(dx, 0.0), std::max(dy, 0.0), std::max(dz, 0.0)) +
           std::min(std::max({dx, dy, dz}), 0.0);
}
using DensityCache = std::map<std::tuple<std::size_t, double, double, double>, double>;
double evaluate(const schema::TerrainGenerationSpecT& spec, const FastNoise::Generator& noise,
                Vec3 p, schema::TerrainLayerSample& layer, std::size_t count, DensityCache& cache,
                double base = std::numeric_limits<double>::quiet_NaN(), std::uint32_t depth = 0)
{
    if (depth > 32)
    {
        return std::numeric_limits<double>::quiet_NaN();
    }
    double height = std::isnan(base) ? base_height(spec, noise, p.x(), p.z()) : base;
    for (const auto& road : spec.roads)
    {
        for (std::size_t i = 1; i < road->points.size(); ++i)
        {
            const auto a = road->points[i - 1], b = road->points[i];
            const double dx = b.x() - a.x(), dz = b.z() - a.z(), length = dx * dx + dz * dz;
            if (length <= 1e-12)
            {
                continue;
            }
            const double t =
                std::clamp(((p.x() - a.x()) * dx + (p.z() - a.z()) * dz) / length, 0.0, 1.0);
            const double distance = std::hypot(p.x() - a.x() - t * dx, p.z() - a.z() - t * dz);
            const double weight =
                road->falloff > 0
                    ? std::clamp((road->width * 0.5 + road->falloff - distance) / road->falloff,
                                 0.0, 1.0)
                    : double(distance <= road->width * 0.5);
            height += (a.y() + t * (b.y() - a.y()) - height) * weight;
            if (weight > 0)
            {
                layer = terrain_detail::blend_layers(layer, road->layer, weight);
            }
        }
    }
    double value = std::clamp(p.y() - height, -spec.voxel_size, spec.voxel_size);
    for (std::size_t i = 0; i < std::min(count, spec.stamps.size()); ++i)
    {
        const auto& stamp = *spec.stamps[i];
        const double d = stamp_distance(stamp, p);
        double target = value;
        switch (stamp.operation)
        {
        case schema::TerrainBrushOperation::dig:
            target = std::max(value, -d);
            break;
        case schema::TerrainBrushOperation::fill:
            target = std::min(value, d);
            break;
        case schema::TerrainBrushOperation::flatten:
            if (d <= 0)
            {
                target = p.y() - stamp.center.y();
            }
            break;
        case schema::TerrainBrushOperation::smooth:
            if (d <= 0)
            {
                const double step = spec.voxel_size;
                target = 0;
                for (const auto& q :
                     {Vec3{p.x() - step, p.y(), p.z()}, Vec3{p.x() + step, p.y(), p.z()},
                      Vec3{p.x(), p.y() - step, p.z()}, Vec3{p.x(), p.y() + step, p.z()},
                      Vec3{p.x(), p.y(), p.z() - step}, Vec3{p.x(), p.y(), p.z() + step}})
                {
                    const auto key = std::make_tuple(i, q.x(), q.y(), q.z());
                    auto found = cache.find(key);
                    if (found == cache.end())
                    {
                        // Bound scratch memory and memoize previous operation samples. A chain of
                        // smooth brushes must not expand into six recursive calls per brush.
                        if (cache.size() >= 262144)
                        {
                            return std::numeric_limits<double>::quiet_NaN();
                        }
                        auto ignored = layer;
                        const auto density =
                            evaluate(spec, noise, q, ignored, i, cache,
                                     std::numeric_limits<double>::quiet_NaN(), depth + 1);
                        if (!std::isfinite(density))
                        {
                            return density;
                        }
                        found = cache.emplace(key, density).first;
                    }
                    target += found->second / 6;
                }
            }
            break;
        case schema::TerrainBrushOperation::paint:
            break;
        }
        value = std::clamp(value + (target - value) * stamp.strength, -spec.voxel_size,
                           spec.voxel_size);
        if (d <= 0 && (stamp.operation == schema::TerrainBrushOperation::paint ||
                       stamp.operation == schema::TerrainBrushOperation::fill))
        {
            layer = terrain_detail::blend_layers(layer, stamp.layer, stamp.strength);
        }
    }
    return value;
}
class NoiseTerrainGenerator final : public TerrainGenerator
{
public:
    bool generate(const TerrainGenerationInput& input, schema::TerrainBlockDataT& output,
                  std::string& error) const override
    {
        output = {};
        error.clear();
        const auto& spec = input.recipe;
        if (!terrain_detail::valid_bounds(spec.bounds) || input.key.lod() >= spec.lod_count ||
            spec.voxel_size <= 0 ||
            (spec.block_cells != 8 && spec.block_cells != 16 && spec.block_cells != 32))
        {
            error = "Invalid terrain generation input";
            return false;
        }
        if (spec.noise_graph.size() > 65536 ||
            (spec.heightmap &&
             (spec.heightmap->width < 2 || spec.heightmap->height < 2 ||
              spec.heightmap->width > 1024 || spec.heightmap->height > 1024 ||
              spec.heightmap->samples.size() !=
                  std::uint64_t(spec.heightmap->width) * spec.heightmap->height ||
              !std::isfinite(spec.heightmap->height_scale) ||
              !std::isfinite(spec.heightmap->height_offset) ||
              !std::all_of(spec.heightmap->samples.begin(), spec.heightmap->samples.end(),
                           [](float v) { return std::isfinite(v) && v >= 0 && v <= 1; }))))
        {
            error = "Invalid heightmap or noise graph";
            return false;
        }
        FastNoise::SmartNode<> noise;
        if (!spec.noise_graph.empty())
        {
            noise = FastNoise::NewFromEncodedNodeTree(spec.noise_graph.c_str());
        }
        else
        {
            auto fractal = FastNoise::New<FastNoise::FractalFBm>();
            auto simplex = FastNoise::New<FastNoise::Simplex>();
            if (!fractal || !simplex)
            {
                error = "No supported noise SIMD backend";
                return false;
            }
            // Recipe frequency already scales the domain; FastNoise defaults to scale 100.
            simplex->SetScale(1.0F);
            fractal->SetSource(simplex);
            fractal->SetOctaveCount(4);
            fractal->SetGain(.5F);
            noise = fractal;
        }
        if (!noise)
        {
            error = "Invalid FastNoise2 encoded node tree";
            return false;
        }
        schema::TerrainBlockDataT block;
        block.key = input.key;
        const auto n = spec.block_cells;
        const double spacing = std::ldexp(spec.voxel_size, input.key.lod());
        double minimum = spec.height - spec.amplitude, maximum = spec.height + spec.amplitude;
        if (spec.heightmap)
        {
            minimum += spec.heightmap->height_offset + std::min(0.0, spec.heightmap->height_scale);
            maximum += spec.heightmap->height_offset + std::max(0.0, spec.heightmap->height_scale);
        }
        for (const auto& road : spec.roads)
        {
            if (!road)
            {
                error = "Missing terrain road";
                return false;
            }
            for (auto p : road->points)
            {
                minimum = std::min(minimum, p.y());
                maximum = std::max(maximum, p.y());
            }
        }
        double smooth_margin = 0;
        for (const auto& stamp : spec.stamps)
        {
            if (!stamp)
            {
                error = "Missing terrain stamp";
                return false;
            }
            const auto radius = stamp->shape == schema::TerrainBrushShape::sphere
                                    ? stamp->extent.x()
                                    : stamp->extent.y();
            minimum = std::min(minimum, stamp->center.y() - radius - spacing);
            maximum = std::max(maximum, stamp->center.y() + radius + spacing);
            if (stamp->operation == schema::TerrainBrushOperation::smooth)
            {
                smooth_margin += spec.voxel_size;
            }
        }
        minimum -= smooth_margin;
        maximum += smooth_margin;
        const double first_y = spec.origin.y() + std::int64_t(input.key.y()) * n * spacing;
        const double last_y = first_y + (n - 1) * spacing;
        const schema::TerrainLayerSample default_layer{0, 0, 0, 0, Vec4{1, 0, 0, 0}};
        if (first_y >= maximum + spec.voxel_size || last_y <= minimum - spec.voxel_size)
        {
            block.uniform = true;
            block.density.push_back(
                float(first_y >= maximum + spec.voxel_size ? spec.voxel_size : -spec.voxel_size));
            block.layers.push_back(default_layer);
            output = std::move(block);
            return true;
        }
        std::vector<double> heights(std::size_t(n) * n);
        for (std::uint32_t z = 0; z < n; ++z)
        {
            for (std::uint32_t x = 0; x < n; ++x)
            {
                heights[z * n + x] = base_height(
                    spec, *noise, spec.origin.x() + (std::int64_t(input.key.x()) * n + x) * spacing,
                    spec.origin.z() + (std::int64_t(input.key.z()) * n + z) * spacing);
            }
        }
        block.density.reserve(std::size_t(n) * n * n);
        block.layers.reserve(std::size_t(n) * n * n);
        DensityCache cache;
        for (std::uint32_t z = 0; z < n; ++z)
        {
            for (std::uint32_t y = 0; y < n; ++y)
            {
                for (std::uint32_t x = 0; x < n; ++x)
                {
                    Vec3 p{spec.origin.x() + (std::int64_t(input.key.x()) * n + x) * spacing,
                           spec.origin.y() + (std::int64_t(input.key.y()) * n + y) * spacing,
                           spec.origin.z() + (std::int64_t(input.key.z()) * n + z) * spacing};
                    schema::TerrainLayerSample layer{0, 0, 0, 0, Vec4{1, 0, 0, 0}};
                    const auto density = evaluate(spec, *noise, p, layer, spec.stamps.size(), cache,
                                                  heights[z * n + x]);
                    if (!std::isfinite(density) ||
                        !terrain_detail::layer_valid(layer, spec.material_layers.size()))
                    {
                        error = "Terrain generation produced invalid samples or exceeded its "
                                "smooth sample budget";
                        return false;
                    }
                    block.density.push_back(
                        float(std::clamp(density, -spec.voxel_size, spec.voxel_size)));
                    block.layers.push_back(layer);
                }
            }
        }
        if (std::all_of(block.density.begin(), block.density.end(),
                        [&](float v) { return v == block.density.front(); }) &&
            std::all_of(block.layers.begin(), block.layers.end(),
                        [&](const auto& v) { return v == block.layers.front(); }))
        {
            block.uniform = true;
            block.density.resize(1);
            block.layers.resize(1);
        }
        output = std::move(block);
        return true;
    }
};
} // namespace
std::unique_ptr<TerrainGenerator> create_terrain_generator(std::string& error)
{
    error.clear();
    return std::make_unique<NoiseTerrainGenerator>();
}
} // namespace engine::plugins::terrain
