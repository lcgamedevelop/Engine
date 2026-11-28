#include "editor/mesh/mesh_lods.hpp"
#include "runtime/performance/function_profile.hpp"
#include "runtime/performance/profiling.hpp"
#include "editor/mesh/mesh_lod_settings.hpp"

#include "runtime/resource/mesh_storage.hpp"
#include "runtime/utils/mesh_utils.hpp"

#include <meshoptimizer.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <span>

namespace engine::editor
{
namespace
{

bool sequence(const toml::table& table, std::string_view name, std::vector<float>& output,
              std::string& error)
{
    ENGINE_PROFILE_FUNCTION();
    if (!table.contains(name))
    {
        return true;
    }
    const auto* array = table[name].as_array();
    if (!array || array->empty())
    {
        error = "lod." + std::string(name) + " must be a nonempty number array";
        return false;
    }
    std::vector<float> values;
    float previous = name == "screen_heights" ? std::nextafter(1.0F, 2.0F) : 1.0F;
    for (const auto& item : *array)
    {
        const auto value = item.value<double>();
        const float converted = value ? static_cast<float>(*value) : 0;
        if (!value || !std::isfinite(converted) || converted <= 0 || converted >= previous)
        {
            error = "lod." + std::string(name) + " must strictly decrease in (0,1]";
            return false;
        }
        values.push_back(converted);
        previous = converted;
    }
    output.swap(values);
    return true;
}

} // namespace

bool read_mesh_lod_settings(const toml::table& meta, MeshLodSettings& output, std::string& error)
{
    ENGINE_PROFILE_FUNCTION();
    MeshLodSettings settings;
    if (!meta.contains("lod"))
    {
        output = std::move(settings);
        return true;
    }
    const auto* table = meta["lod"].as_table();
    if (!table)
    {
        error = "lod must be a table";
        return false;
    }
    for (const auto& [key, unused] : *table)
    {
        (void)unused;
        if (key != "enabled" && key != "triangle_ratios" && key != "screen_heights" &&
            key != "max_relative_error")
        {
            error = "unknown lod setting: " + std::string(key.str());
            return false;
        }
    }
    if (table->contains("enabled") && !(*table)["enabled"].is_boolean())
    {
        error = "lod.enabled must be boolean";
        return false;
    }
    settings.enabled = (*table)["enabled"].value_or(false);
    if (!sequence(*table, "triangle_ratios", settings.triangle_ratios, error) ||
        !sequence(*table, "screen_heights", settings.screen_heights, error) ||
        settings.triangle_ratios.size() != settings.screen_heights.size())
    {
        if (error.empty())
        {
            error = "lod triangle_ratios and screen_heights must have equal length";
        }
        return false;
    }
    if (table->contains("max_relative_error"))
    {
        const auto value = (*table)["max_relative_error"].value<double>();
        if (!value || !std::isfinite(*value) || *value <= 0 || *value > 1)
        {
            error = "lod.max_relative_error must be finite and in (0,1]";
            return false;
        }
        settings.max_relative_error = static_cast<float>(*value);
        if (settings.max_relative_error <= 0)
        {
            error = "lod.max_relative_error cannot underflow single precision";
            return false;
        }
    }
    output = std::move(settings);
    return true;
}

bool generate_mesh_lods(schema::MeshDataT& mesh, const toml::table& meta, std::string& error)
{
    ENGINE_PROFILE_FUNCTION();
    MeshLodSettings settings;
    if (!read_mesh_lod_settings(meta, settings, error))
    {
        return false;
    }
    if (!MeshUtils::validate(mesh))
    {
        error = "Cannot generate LODs for invalid mesh data";
        return false;
    }
    if (!settings.enabled || !mesh.skin.empty() || !mesh.skeleton_guid.empty())
    {
        mesh.lods.clear();
        return true;
    }
    // Preserve original vertices/attributes. Non-permissive simplification and explicit
    // seam locks prevent collapses across independently shaded UV/normal/tangent charts.
    std::vector<float> attributes;
    std::vector<float> weights{0.5F, 0.5F, 0.5F};
    const bool has_uv = mesh_storage::texcoord_count(mesh) != 0;
    const bool has_lightmap_uv = mesh_storage::lightmap_texcoord_count(mesh) != 0;
    const bool has_tangents = mesh_storage::tangent_count(mesh) != 0;
    const auto extend = [&](std::size_t count, float weight)
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        weights.insert(weights.end(), count, weight);
    };
    if (has_uv)
    {
        extend(2, 1.0F);
    }
    if (has_lightmap_uv)
    {
        extend(2, 1.0F);
    }
    if (has_tangents)
    {
        extend(4, 0.25F);
    }
    if (!mesh.colors.empty())
    {
        extend(4, 1.0F);
    }
    const auto attribute_count = weights.size();
    attributes.reserve(mesh.vertices.size() * attribute_count);
    std::vector<std::array<float, 3>> vertex_positions;
    vertex_positions.reserve(mesh.vertices.size());
    std::map<std::array<float, 3>, std::vector<std::uint32_t>> positions;
    for (std::size_t index = 0; index < mesh.vertices.size(); ++index)
    {
        const auto& vertex = mesh.vertices[index];
        vertex_positions.push_back({vertex.x(), vertex.y(), vertex.z()});
        positions[{vertex.x(), vertex.y(), vertex.z()}].push_back(
            static_cast<std::uint32_t>(index));
        const double length =
            std::hypot(static_cast<double>(vertex.nx()), static_cast<double>(vertex.ny()),
                       static_cast<double>(vertex.nz()));
        attributes.insert(attributes.end(), {static_cast<float>(vertex.nx() / length),
                                             static_cast<float>(vertex.ny() / length),
                                             static_cast<float>(vertex.nz() / length)});
        const auto append_uv = [&](schema::Vec2 uv)
        {
            ENGINE_PROFILE_FUNCTION_PROBE();
            attributes.push_back(static_cast<float>(uv.x()));
            attributes.push_back(static_cast<float>(uv.y()));
        };
        if (has_uv)
        {
            append_uv(mesh_storage::texcoord(mesh, index));
        }
        if (has_lightmap_uv)
        {
            append_uv(mesh_storage::lightmap_texcoord(mesh, index));
        }
        if (has_tangents)
        {
            const auto tangent = mesh_storage::tangent(mesh, index);
            attributes.insert(attributes.end(),
                              {static_cast<float>(tangent.x()), static_cast<float>(tangent.y()),
                               static_cast<float>(tangent.z()), static_cast<float>(tangent.w())});
        }
        if (!mesh.colors.empty())
        {
            const auto& color = mesh.colors[index];
            attributes.insert(attributes.end(), {color.r(), color.g(), color.b(), color.a()});
        }
    }
    std::vector<unsigned char> locks(mesh.vertices.size());
    for (const auto& [position, coincident] : positions)
    {
        (void)position;
        if (coincident.size() < 2)
        {
            continue;
        }
        const auto first = attributes.begin() + coincident.front() * attribute_count;
        const bool seam =
            std::any_of(coincident.begin() + 1, coincident.end(),
                        [&](auto index)
                        {
                            ENGINE_PROFILE_FUNCTION_PROBE();
                            return !std::equal(first, first + attribute_count,
                                               attributes.begin() + index * attribute_count);
                        });
        if (seam)
        {
            for (const auto index : coincident)
            {
                locks[index] = meshopt_SimplifyVertex_Lock;
            }
        }
    }
    const auto topology = mesh_storage::indices(mesh);
    std::vector<std::uint32_t> widened;
    auto source_indices = topology.wide;
    if (!topology.narrow.empty())
    {
        topology.copy_to(widened);
        source_indices = widened;
    }
    std::vector<schema::MeshSubmesh> parts = mesh.submeshes;
    if (parts.empty())
    {
        parts.emplace_back(0, static_cast<std::uint32_t>(source_indices.size()), 0);
    }
    std::vector<std::unique_ptr<schema::MeshLodDataT>> levels;
    std::size_t previous_count = source_indices.size();
    for (std::size_t level = 0; level < settings.triangle_ratios.size(); ++level)
    {
        auto lod = std::make_unique<schema::MeshLodDataT>();
        lod->screen_height = settings.screen_heights[level];
        for (const auto& part : parts)
        {
            std::vector<std::uint32_t> indices(part.index_count());
            const std::size_t target =
                std::max<std::size_t>(1,
                                      static_cast<std::size_t>(part.index_count() / 3 *
                                                               settings.triangle_ratios[level])) *
                3;
            const auto count = meshopt_simplifyWithAttributes(
                indices.data(), source_indices.data() + part.first_index(), part.index_count(),
                vertex_positions.front().data(), mesh.vertices.size(), sizeof(vertex_positions[0]),
                attributes.data(), attribute_count * sizeof(float), weights.data(), attribute_count,
                locks.data(), target, settings.max_relative_error, meshopt_SimplifyLockBorder,
                nullptr);
            // Keep a material group intact if no useful simplification was possible.
            if (count == 0)
            {
                indices.assign(source_indices.begin() + part.first_index(),
                               source_indices.begin() + part.first_index() + part.index_count());
            }
            else
            {
                indices.resize(count);
            }
            meshopt_optimizeVertexCache(indices.data(), indices.data(), indices.size(),
                                        mesh.vertices.size());
            lod->submeshes.emplace_back(static_cast<std::uint32_t>(lod->indices.size()),
                                        static_cast<std::uint32_t>(indices.size()),
                                        part.material_slot());
            lod->indices.insert(lod->indices.end(), indices.begin(), indices.end());
        }
        if (lod->indices.size() < previous_count)
        {
            previous_count = lod->indices.size();
            levels.push_back(std::move(lod));
        }
    }
    auto candidate = mesh;
    candidate.lods = std::move(levels);
    if (!MeshUtils::validate(candidate))
    {
        error = "Generated mesh LOD topology failed validation";
        return false;
    }
    mesh.lods.swap(candidate.lods);
    return true;
}

bool generate_mesh_lods(schema::MeshDataT& mesh, const std::filesystem::path& source,
                        std::string& error)
{
    ENGINE_PROFILE_FUNCTION();
    auto path = source;
    path += ".meta";
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
    {
        error = "Cannot read mesh LOD source meta";
        return false;
    }
    auto parsed = toml::parse(stream);
    if (!parsed)
    {
        error = "Invalid mesh LOD source meta: " + std::string(parsed.error().description());
        return false;
    }
    return generate_mesh_lods(mesh, parsed.table(), error);
}

std::string mesh_lod_fingerprint()
{
    ENGINE_PROFILE_FUNCTION();
    return ":meshoptimizer-" + std::to_string(MESHOPTIMIZER_VERSION) + ":static-lod-1";
}

} // namespace engine::editor
