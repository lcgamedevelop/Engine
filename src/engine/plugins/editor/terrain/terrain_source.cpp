#include "plugins/editor/terrain/terrain_source.hpp"
#include "runtime/performance/function_profile.hpp"
#include "runtime/performance/profiling.hpp"
#include "editor/asset_pipeline/toml_helpers.hpp"
#include "editor/file_text.hpp"
#include "plugins/runtime/terrain/terrain_samples.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <limits>
#include <sstream>

namespace engine::plugins::terrain
{
namespace
{
bool fields(const toml::table& table, std::initializer_list<std::string_view> allowed,
            std::string& error)
{
    ENGINE_PROFILE_FUNCTION();
    for (const auto& [key, value] : table)
    {
        (void)value;
        if (std::find(allowed.begin(), allowed.end(), key.str()) == allowed.end())
        {
            error = "Unknown terrain source field: " + std::string(key.str());
            return false;
        }
    }
    return true;
}
bool vector(const toml::node* node, Vec3& output)
{
    ENGINE_PROFILE_FUNCTION();
    std::array<double, 3> values{};
    if (!editor::asset_pipeline::numbers(node, values))
    {
        return false;
    }
    output = {values[0], values[1], values[2]};
    return true;
}
bool number(const toml::table& table, std::string_view name, double& output)
{
    ENGINE_PROFILE_FUNCTION();
    if (!table.contains(name))
    {
        return true;
    }
    const auto value = table[name].value<double>();
    if (!value || !std::isfinite(*value))
    {
        return false;
    }
    output = *value;
    return true;
}
bool brushes(const toml::node* node,
             std::vector<std::unique_ptr<schema::TerrainBrushDataT>>& output, std::string& error)
{
    ENGINE_PROFILE_FUNCTION();
    if (!node)
    {
        return true;
    }
    const auto* array = node->as_array();
    if (!array || array->size() > 4096)
    {
        error = "Invalid terrain operation stack";
        return false;
    }
    for (const auto& value : *array)
    {
        const auto* table = value.as_table();
        if (!table ||
            !fields(*table, {"center", "extent", "shape", "operation", "strength", "layer"}, error))
        {
            return false;
        }
        auto brush = std::make_unique<schema::TerrainBrushDataT>();
        if ((table->contains("shape") && !(*table)["shape"].is_string()) ||
            (table->contains("operation") && !(*table)["operation"].is_string()) ||
            (table->contains("layer") && !(*table)["layer"].is_integer()))
        {
            error = "Terrain brush shape and operation must be strings and layer an integer";
            return false;
        }
        if (!vector(table->get("center"), brush->center) ||
            !vector(table->get("extent"), brush->extent) || brush->extent.x() <= 0 ||
            brush->extent.y() <= 0 || brush->extent.z() <= 0)
        {
            error = "Invalid terrain brush bounds";
            return false;
        }
        const auto shape = (*table)["shape"].value_or(std::string("sphere"));
        if (shape == "sphere")
        {
            brush->shape = schema::TerrainBrushShape::sphere;
        }
        else if (shape == "box")
        {
            brush->shape = schema::TerrainBrushShape::box;
        }
        else
        {
            error = "Unknown terrain brush shape";
            return false;
        }
        const auto operation = (*table)["operation"].value_or(std::string("dig"));
        bool matched = false;
        for (auto candidate : schema::EnumValuesTerrainBrushOperation())
        {
            if (operation == schema::EnumNameTerrainBrushOperation(candidate))
            {
                brush->operation = candidate;
                matched = true;
                break;
            }
        }
        if (!matched)
        {
            error = "Unknown terrain brush operation";
            return false;
        }
        double strength = 1;
        if (!number(*table, "strength", strength) || strength <= 0 || strength > 1)
        {
            error = "Invalid brush strength";
            return false;
        }
        brush->strength = float(strength);
        const auto layer = (*table)["layer"].value_or(std::int64_t(0));
        if (layer < 0 || layer > 255)
        {
            error = "Invalid terrain material layer";
            return false;
        }
        brush->layer = {std::uint32_t(layer), 0, 0, 0, Vec4{1, 0, 0, 0}};
        output.push_back(std::move(brush));
    }
    return true;
}
} // namespace
bool read_terrain_source(std::string_view text, schema::TerrainSourceDataT& output,
                         std::string& error)
{
    ENGINE_PROFILE_FUNCTION();
    output = {};
    error.clear();
    if (text.empty() || text.size() > 4 * 1024 * 1024)
    {
        error = "Terrain source exceeds 4 MiB";
        return false;
    }
    const auto parsed = toml::parse(text);
    if (!parsed)
    {
        error = "Invalid terrain TOML: " + std::string(parsed.error().description());
        return false;
    }
    const auto& table = parsed.table();
    for (const auto* key : {"schema_version", "revision", "block_cells", "lod_count", "seed"})
    {
        if (table.contains(key) && !table[key].is_integer())
        {
            error = "Terrain field must be an integer: " + std::string(key);
            return false;
        }
    }
    for (const auto* key : {"material_layers", "stamps", "edits", "roads"})
    {
        if (table.contains(key) && !table[key].is_array())
        {
            error = "Terrain field must be an array: " + std::string(key);
            return false;
        }
    }
    if (!fields(table,
                {"schema_version", "revision", "origin", "bounds_min", "bounds_max", "voxel_size",
                 "block_cells", "lod_count", "seed", "height", "amplitude", "frequency",
                 "noise_graph", "heightmap", "material_layers", "stamps", "edits", "roads"},
                error))
    {
        return false;
    }
    if (table["schema_version"].value_or(std::int64_t(0)) != 2)
    {
        error = "Terrain source schema_version must be 2";
        return false;
    }
    const auto revision = table["revision"].value_or(std::int64_t(1));
    if (revision <= 0)
    {
        error = "Invalid terrain source revision";
        return false;
    }
    auto recipe = std::make_unique<schema::TerrainGenerationSpecT>();
    Vec3 low = recipe->bounds.minimum(), high = recipe->bounds.maximum();
    if ((table.contains("origin") && !vector(table.get("origin"), recipe->origin)) ||
        !vector(table.get("bounds_min"), low) || !vector(table.get("bounds_max"), high))
    {
        error = "Terrain requires finite bounds_min and bounds_max";
        return false;
    }
    recipe->bounds = {low, high};
    const auto cells = table["block_cells"].value_or(std::int64_t(16));
    const auto lods = table["lod_count"].value_or(std::int64_t(4));
    const auto seed = table["seed"].value_or(std::int64_t(1));
    if ((cells != 8 && cells != 16 && cells != 32) || lods < 1 || lods > 16 || seed < 0 ||
        seed > UINT32_MAX || !terrain_detail::valid_bounds(recipe->bounds) ||
        !number(table, "voxel_size", recipe->voxel_size) || recipe->voxel_size < 0.01 ||
        recipe->voxel_size > 1024 || !number(table, "height", recipe->height) ||
        !number(table, "amplitude", recipe->amplitude) || recipe->amplitude < 0 ||
        !number(table, "frequency", recipe->frequency) || recipe->frequency <= 0)
    {
        error = "Invalid terrain dimensions or generation parameters";
        return false;
    }
    recipe->block_cells = std::uint16_t(cells);
    recipe->lod_count = std::uint8_t(lods);
    recipe->seed = std::uint32_t(seed);
    recipe->noise_graph = table["noise_graph"].value_or(std::string{});
    if (recipe->noise_graph.size() > 65536 ||
        (table.contains("noise_graph") && !table["noise_graph"].is_string()))
    {
        error = "Invalid terrain noise graph";
        return false;
    }
    if (const auto* map = table["heightmap"].as_table())
    {
        if (!fields(*map, {"width", "height", "samples", "height_scale", "height_offset"}, error))
        {
            return false;
        }
        auto heightmap = std::make_unique<schema::TerrainHeightmapDataT>();
        const auto width = (*map)["width"].value_or(std::int64_t(0));
        const auto height = (*map)["height"].value_or(std::int64_t(0));
        const auto* samples = (*map)["samples"].as_array();
        if (width < 2 || height < 2 || width > 1024 || height > 1024 || !samples ||
            std::uint64_t(width) * height != samples->size() ||
            !number(*map, "height_scale", heightmap->height_scale) ||
            !number(*map, "height_offset", heightmap->height_offset))
        {
            error = "Invalid terrain heightmap dimensions or scale";
            return false;
        }
        heightmap->width = std::uint32_t(width);
        heightmap->height = std::uint32_t(height);
        for (const auto& sample : *samples)
        {
            const auto value = sample.value<double>();
            if (!value || !std::isfinite(*value) || *value < 0 || *value > 1)
            {
                error = "Heightmap samples must be normalized finite values";
                return false;
            }
            heightmap->samples.push_back(float(*value));
        }
        recipe->heightmap = std::move(heightmap);
    }
    else if (table.contains("heightmap"))
    {
        error = "Heightmap must be a table";
        return false;
    }
    if (const auto* layers = table["material_layers"].as_array())
    {
        for (const auto& layer : *layers)
        {
            auto value = layer.value<std::string>();
            if (!value || value->empty() || recipe->material_layers.size() == 256)
            {
                error = "Invalid terrain material layers";
                return false;
            }
            recipe->material_layers.push_back(*value);
        }
    }
    if (!brushes(table.get("stamps"), recipe->stamps, error) ||
        !brushes(table.get("edits"), output.edits, error))
    {
        return false;
    }
    if (const auto* roads = table["roads"].as_array())
    {
        if (roads->size() > 256)
        {
            error = "Too many terrain roads";
            return false;
        }
        for (const auto& value : *roads)
        {
            const auto* road = value.as_table();
            if (!road || !fields(*road, {"points", "width", "falloff", "layer"}, error))
            {
                return false;
            }
            auto item = std::make_unique<schema::TerrainRoadDataT>();
            const auto* points = (*road)["points"].as_array();
            if (!points || points->size() < 2 || points->size() > 1024 ||
                !number(*road, "width", item->width) || item->width <= 0 ||
                !number(*road, "falloff", item->falloff) || item->falloff < 0)
            {
                error = "Invalid terrain road dimensions";
                return false;
            }
            for (const auto& point : *points)
            {
                Vec3 p;
                if (!vector(&point, p))
                {
                    error = "Invalid road point";
                    return false;
                }
                item->points.push_back(p);
            }
            const auto layer = (*road)["layer"].value_or(std::int64_t(0));
            if ((road->contains("layer") && !(*road)["layer"].is_integer()) || layer < 0 ||
                layer > 255)
            {
                error = "Invalid road layer";
                return false;
            }
            item->layer = {std::uint32_t(layer), 0, 0, 0, Vec4{1, 0, 0, 0}};
            recipe->roads.push_back(std::move(item));
        }
    }
    output.revision = std::uint64_t(revision);
    output.recipe = std::move(recipe);
    std::vector<TerrainRegionKey> keys;
    return terrain_source_keys(output, keys, error);
}
bool write_terrain_source(const schema::TerrainSourceDataT& source, std::string& output,
                          std::string& error)
{
    ENGINE_PROFILE_FUNCTION();
    output.clear();
    if (!source.recipe || source.revision == 0 || source.revision > INT64_MAX)
    {
        error = "Invalid terrain source revision";
        return false;
    }
    const auto vector = [](Vec3 value)
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        return toml::array{value.x(), value.y(), value.z()};
    };
    const auto& recipe = *source.recipe;
    toml::table table;
    table.insert("schema_version", 2);
    table.insert("revision", std::int64_t(source.revision));
    table.insert("origin", vector(recipe.origin));
    table.insert("bounds_min", vector(recipe.bounds.minimum()));
    table.insert("bounds_max", vector(recipe.bounds.maximum()));
    table.insert("voxel_size", recipe.voxel_size);
    table.insert("block_cells", recipe.block_cells);
    table.insert("lod_count", recipe.lod_count);
    table.insert("seed", recipe.seed);
    table.insert("height", recipe.height);
    table.insert("amplitude", recipe.amplitude);
    table.insert("frequency", recipe.frequency);
    table.insert("noise_graph", recipe.noise_graph);
    if (recipe.heightmap)
    {
        const auto& source_map = *recipe.heightmap;
        toml::array samples;
        for (float value : source_map.samples)
        {
            samples.push_back(value);
        }
        table.insert("heightmap", toml::table{{"width", source_map.width},
                                              {"height", source_map.height},
                                              {"height_scale", source_map.height_scale},
                                              {"height_offset", source_map.height_offset},
                                              {"samples", std::move(samples)}});
    }
    toml::array layers;
    for (const auto& layer : recipe.material_layers)
    {
        layers.push_back(layer);
    }
    table.insert("material_layers", std::move(layers));
    const auto brushes = [&](const auto& values)
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        toml::array result;
        for (const auto& value : values)
        {
            if (!value)
            {
                continue;
            }
            toml::table item;
            item.insert("center", vector(value->center));
            item.insert("extent", vector(value->extent));
            item.insert("shape", schema::EnumNameTerrainBrushShape(value->shape));
            item.insert("operation", schema::EnumNameTerrainBrushOperation(value->operation));
            item.insert("strength", value->strength);
            item.insert("layer", value->layer.layer0());
            result.push_back(std::move(item));
        }
        return result;
    };
    table.insert("stamps", brushes(recipe.stamps));
    table.insert("edits", brushes(source.edits));
    toml::array roads;
    for (const auto& value : recipe.roads)
    {
        if (!value)
        {
            continue;
        }
        toml::table road;
        toml::array points;
        for (auto p : value->points)
        {
            points.push_back(vector(p));
        }
        road.insert("points", std::move(points));
        road.insert("width", value->width);
        road.insert("falloff", value->falloff);
        road.insert("layer", value->layer.layer0());
        roads.push_back(std::move(road));
    }
    table.insert("roads", std::move(roads));
    std::ostringstream stream;
    stream << table;
    output = stream.str();
    schema::TerrainSourceDataT parsed;
    if (!read_terrain_source(output, parsed, error) || parsed != source)
    {
        output.clear();
        if (error.empty())
        {
            error = "Terrain source cannot be represented without data loss";
        }
        return false;
    }
    return true;
}
bool read_terrain_file(const std::filesystem::path& path, schema::TerrainSourceDataT& output,
                       std::string& error)
{
    ENGINE_PROFILE_FUNCTION();
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    std::string text;
    if (ec || size > 4 * 1024 * 1024 || !editor::file_detail::read_text(path, text))
    {
        error = "Cannot read terrain source";
        return false;
    }
    return read_terrain_source(text, output, error);
}
bool terrain_source_keys(const schema::TerrainSourceDataT& source,
                         std::vector<TerrainRegionKey>& keys, std::string& error)
{
    ENGINE_PROFILE_FUNCTION();
    keys.clear();
    if (!source.recipe)
    {
        error = "Terrain recipe is missing";
        return false;
    }
    schema::TerrainAssetDataT index;
    index.origin = source.recipe->origin;
    index.bounds = source.recipe->bounds;
    index.voxel_size = source.recipe->voxel_size;
    index.block_cells = source.recipe->block_cells;
    for (std::uint8_t lod = 0; lod < source.recipe->lod_count; ++lod)
    {
        std::vector<TerrainRegionKey> level;
        if (!terrain_detail::enumerate(index, index.bounds, lod, 65536, level) ||
            keys.size() + level.size() > 65536)
        {
            error =
                "Terrain source exceeds 65536 authored regions; increase block size or voxel size";
            return false;
        }
        keys.insert(keys.end(), level.begin(), level.end());
    }
    return !keys.empty();
}
std::string terrain_region_name(TerrainRegionKey key)
{
    ENGINE_PROFILE_FUNCTION();
    return "regions/" + std::to_string(key.lod()) + "/" + std::to_string(key.x()) + "_" +
           std::to_string(key.y()) + "_" + std::to_string(key.z());
}
bool terrain_region_key(std::string_view name, TerrainRegionKey& output)
{
    ENGINE_PROFILE_FUNCTION();
    output = {};
    if (!name.starts_with("regions/"))
    {
        return false;
    }
    name.remove_prefix(8);
    std::array<std::int32_t, 4> values{};
    for (std::size_t i = 0; i < 4; ++i)
    {
        const auto end = i == 0 ? name.find('/') : name.find('_');
        const auto part = (i == 3) ? name : name.substr(0, end);
        if (part.empty() || (i < 3 && end == std::string_view::npos))
        {
            return false;
        }
        const auto result = std::from_chars(part.data(), part.data() + part.size(), values[i]);
        if (result.ec != std::errc{} || result.ptr != part.data() + part.size())
        {
            return false;
        }
        if (i < 3)
        {
            name.remove_prefix(end + 1);
        }
    }
    if (values[0] < 0 || values[0] > 15)
    {
        return false;
    }
    output = {values[1], values[2], values[3], std::uint8_t(values[0])};
    return true;
}
} // namespace engine::plugins::terrain
