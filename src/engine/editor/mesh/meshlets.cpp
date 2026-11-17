#include "editor/mesh/meshlets.hpp"

#include "runtime/utils/mesh_utils.hpp"

#include <meshoptimizer.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <memory>
#include <span>
#include <vector>

namespace engine::editor
{
namespace
{
constexpr std::size_t maximum_vertices = 64;
constexpr std::size_t maximum_triangles = 124;

bool build(const std::vector<schema::MeshVertex>& vertices,
           const std::vector<std::uint32_t>& indices,
           const std::vector<schema::MeshSubmesh>& submeshes,
           std::unique_ptr<schema::MeshletDataT>& output)
{
    auto result = std::make_unique<schema::MeshletDataT>();
    const auto append = [&](std::span<const std::uint32_t> part)
    {
        const auto bound =
            meshopt_buildMeshletsBound(part.size(), maximum_vertices, maximum_triangles);
        if (bound > std::numeric_limits<std::uint32_t>::max() / (maximum_triangles * 3))
        {
            return false;
        }
        std::vector<meshopt_Meshlet> clusters(bound);
        std::vector<unsigned int> remap(bound * maximum_vertices);
        std::vector<unsigned char> triangles(bound * maximum_triangles * 3);
        const auto* positions = reinterpret_cast<const float*>(vertices.data());
        // Keep the ordinary triangle order, including equal-depth surface ties.
        // Cluster bounds still come from meshoptimizer; no runtime clustering is needed.
        const auto count = meshopt_buildMeshletsScan(
            clusters.data(), remap.data(), triangles.data(), part.data(), part.size(),
            vertices.size(), maximum_vertices, maximum_triangles);
        result->submeshes.emplace_back(static_cast<std::uint32_t>(result->meshlets.size()),
                                       static_cast<std::uint32_t>(count));
        for (std::size_t i = 0; i < count; ++i)
        {
            const auto& cluster = clusters[i];
            const auto bounds = meshopt_computeMeshletBounds(
                remap.data() + cluster.vertex_offset, triangles.data() + cluster.triangle_offset,
                cluster.triangle_count, positions, vertices.size(), sizeof(schema::MeshVertex));
            result->meshlets.emplace_back(
                static_cast<std::uint32_t>(result->indices.size()), cluster.triangle_count * 3,
                schema::Vec3(bounds.center[0], bounds.center[1], bounds.center[2]), bounds.radius,
                schema::Vec3(bounds.cone_axis[0], bounds.cone_axis[1], bounds.cone_axis[2]),
                bounds.cone_cutoff);
            for (std::size_t triangle = 0; triangle < cluster.triangle_count * 3; ++triangle)
            {
                result->indices.push_back(
                    remap[cluster.vertex_offset + triangles[cluster.triangle_offset + triangle]]);
            }
        }
        return true;
    };
    if (submeshes.empty())
    {
        if (!append(indices))
        {
            return false;
        }
    }
    else
    {
        for (const auto& part : submeshes)
        {
            if (!append(std::span(indices).subspan(part.first_index(), part.index_count())))
            {
                return false;
            }
        }
    }
    output = std::move(result);
    return true;
}
} // namespace

bool generate_meshlets(schema::MeshDataT& mesh, const toml::table& meta, std::string& error)
{
    error.clear();
    bool enabled = false;
    if (meta.contains("meshlets"))
    {
        const auto* settings = meta["meshlets"].as_table();
        if (!settings)
        {
            error = "Meshlet settings must be a table";
            return false;
        }
        for (const auto& [key, value] : *settings)
        {
            if (key.str() != "enabled" || !value.is_boolean())
            {
                error = "Meshlet settings only accept a boolean enabled field";
                return false;
            }
        }
        enabled = (*settings)["enabled"].value_or(false);
    }
    if (!MeshUtils::validate(mesh))
    {
        error = "Cannot generate meshlets for invalid mesh data";
        return false;
    }
    auto candidate = mesh;
    candidate.meshlets.reset();
    for (auto& lod : candidate.lods)
    {
        lod->meshlets.reset();
    }
    if (enabled && mesh.skin.empty() && mesh.skeleton_guid.empty())
    {
        if (!build(mesh.vertices, mesh.indices, mesh.submeshes, candidate.meshlets))
        {
            error = "Meshlet generation exceeds the index capacity";
            return false;
        }
        for (auto& lod : candidate.lods)
        {
            if (!build(mesh.vertices, lod->indices, lod->submeshes, lod->meshlets))
            {
                error = "Meshlet LOD generation exceeds the index capacity";
                return false;
            }
        }
    }
    if (!MeshUtils::validate(candidate))
    {
        error = "Generated meshlets failed topology or bounds validation";
        return false;
    }
    mesh.meshlets.swap(candidate.meshlets);
    for (std::size_t i = 0; i < mesh.lods.size(); ++i)
    {
        mesh.lods[i]->meshlets.swap(candidate.lods[i]->meshlets);
    }
    return true;
}

bool generate_meshlets(schema::MeshDataT& mesh, const std::filesystem::path& source,
                       std::string& error)
{
    auto path = source;
    path += ".meta";
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
    {
        error = "Cannot read meshlet source meta";
        return false;
    }
    auto parsed = toml::parse(stream);
    if (!parsed)
    {
        error = "Invalid meshlet source meta: " + std::string(parsed.error().description());
        return false;
    }
    return generate_meshlets(mesh, parsed.table(), error);
}

std::string meshlet_fingerprint()
{
    return ":meshoptimizer-" + std::to_string(MESHOPTIMIZER_VERSION) + ":meshlets-scan-64-124-2";
}

} // namespace engine::editor
