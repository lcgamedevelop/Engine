#include "plugins/runtime/terrain/terrain_mesher.hpp"

#include "plugins/runtime/terrain/terrain_samples.hpp"
#include "runtime/performance/profiling.hpp"

// Eric Lengyel's fixed Transvoxel tables; see third_party/transvoxel and its notice.
#include <Transvoxel.cpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <utility>
#include <vector>

namespace engine::plugins::terrain
{
namespace
{
using Point = std::array<double, 3>;
struct Corner
{
    Point position{};
    Point normal{};
    float density = 0;
    schema::TerrainLayerSample layer;
};

void append_vertex(schema::MeshDataT& mesh, std::vector<std::map<std::uint32_t, double>>& layers,
                   const Corner& a, const Corner& b, double spacing)
{
    const double divisor = static_cast<double>(a.density) - b.density;
    const double t = divisor != 0 ? std::clamp(a.density / divisor, 0.0, 1.0) : 0.5;
    Point position{}, normal{};
    for (std::size_t axis = 0; axis < 3; ++axis)
    {
        position[axis] = (a.position[axis] + (b.position[axis] - a.position[axis]) * t) * spacing;
        normal[axis] = a.normal[axis] + (b.normal[axis] - a.normal[axis]) * t;
    }
    const double length = std::hypot(normal[0], normal[1], normal[2]);
    if (length > 1e-12)
    {
        for (auto& value : normal)
        {
            value /= length;
        }
    }
    else
    {
        normal = {0, 1, 0};
    }
    mesh.vertices.emplace_back(static_cast<float>(position[0]), static_cast<float>(position[1]),
                               static_cast<float>(position[2]), static_cast<float>(normal[0]),
                               static_cast<float>(normal[1]), static_cast<float>(normal[2]));
    mesh.texcoords.emplace_back(position[0], position[2]);
    std::map<std::uint32_t, double> weights;
    for (const auto& [layer, blend] : {std::pair{a.layer, 1 - t}, std::pair{b.layer, t}})
    {
        const auto& w = layer.weights();
        const std::array ids{layer.layer0(), layer.layer1(), layer.layer2(), layer.layer3()};
        const std::array values{w.x(), w.y(), w.z(), w.w()};
        for (std::size_t i = 0; i < 4; ++i)
        {
            weights[ids[i]] += values[i] * blend;
        }
    }
    layers.push_back(std::move(weights));
}

void append_triangle(schema::MeshDataT& mesh, std::uint32_t a, std::uint32_t b, std::uint32_t c)
{
    const auto& av = mesh.vertices[a];
    const auto& bv = mesh.vertices[b];
    const auto& cv = mesh.vertices[c];
    const Point ab{bv.x() - av.x(), bv.y() - av.y(), bv.z() - av.z()};
    const Point ac{cv.x() - av.x(), cv.y() - av.y(), cv.z() - av.z()};
    const Point normal{ab[1] * ac[2] - ab[2] * ac[1], ab[2] * ac[0] - ab[0] * ac[2],
                       ab[0] * ac[1] - ab[1] * ac[0]};
    if (std::hypot(normal[0], normal[1], normal[2]) <= 1e-12)
    {
        return;
    }
    if (normal[0] * (av.nx() + bv.nx() + cv.nx()) + normal[1] * (av.ny() + bv.ny() + cv.ny()) +
            normal[2] * (av.nz() + bv.nz() + cv.nz()) <
        0)
    {
        std::swap(b, c);
    }
    mesh.indices.insert(mesh.indices.end(), {a, b, c});
}

class TransvoxelMesher final : public TerrainMesher
{
public:
    bool build(const TerrainMeshInput& input, TerrainMeshOutput& output,
               std::string& error) const override
    {
        ENGINE_PROFILE_ZONE("Terrain.mesh_region");
        output = {};
        error.clear();
        if (!input.samples.source || input.samples.source_revision == 0 || input.key.lod() > 15 ||
            input.transition_faces > 63 || (input.collision_required && input.key.lod() != 0) ||
            !terrain_detail::resident(input.samples, input.key))
        {
            error = "Invalid terrain mesh input or unavailable region";
            return false;
        }
        const auto cancelled = [&]()
        {
            if (input.cancel_requested && input.cancel_requested->load(std::memory_order_relaxed))
            {
                error = "Terrain meshing cancelled";
                return true;
            }
            return false;
        };
        const auto& source = *input.samples.source;
        const int cells = static_cast<int>(source.block_cells);
        const int axis = cells + 3;
        std::vector<float> grid(static_cast<std::size_t>(axis) * axis * axis);
        std::vector<schema::TerrainLayerSample> layer_grid(grid.size());
        std::vector<std::map<std::uint32_t, double>> vertex_layers;
        const auto index = [axis](int x, int y, int z)
        { return (static_cast<std::size_t>(z + 1) * axis + y + 1) * axis + x + 1; };
        for (int z = -1; z <= cells + 1; ++z)
        {
            if (cancelled())
            {
                return false;
            }
            for (int y = -1; y <= cells + 1; ++y)
            {
                for (int x = -1; x <= cells + 1; ++x)
                {
                    if (!terrain_detail::sample(
                            input.samples, static_cast<std::int64_t>(input.key.x()) * cells + x,
                            static_cast<std::int64_t>(input.key.y()) * cells + y,
                            static_cast<std::int64_t>(input.key.z()) * cells + z, input.key.lod(),
                            grid[index(x, y, z)], &layer_grid[index(x, y, z)]))
                    {
                        error = "Terrain meshing requires a complete same-revision halo";
                        return false;
                    }
                }
            }
        }
        const auto corner = [&](int x, int y, int z)
        {
            Corner result;
            result.position = {static_cast<double>(x), static_cast<double>(y),
                               static_cast<double>(z)};
            result.density = grid[index(x, y, z)];
            result.layer = layer_grid[index(x, y, z)];
            result.normal = {grid[index(x + 1, y, z)] - grid[index(x - 1, y, z)],
                             grid[index(x, y + 1, z)] - grid[index(x, y - 1, z)],
                             grid[index(x, y, z + 1)] - grid[index(x, y, z - 1)]};
            return result;
        };
        const double spacing = std::ldexp(source.voxel_size, input.key.lod());
        auto mesh = std::make_shared<schema::MeshDataT>();
        mesh->path = "builtin://terrain/region.mesh";
        for (int z = 0; z < cells; ++z)
        {
            if (cancelled())
            {
                return false;
            }
            for (int y = 0; y < cells; ++y)
            {
                for (int x = 0; x < cells; ++x)
                {
                    std::array<Corner, 8> points;
                    unsigned mask = 0;
                    for (unsigned i = 0; i < 8; ++i)
                    {
                        points[i] =
                            corner(x + (i & 1U), y + ((i >> 1U) & 1U), z + ((i >> 2U) & 1U));
                        if (points[i].density < 0)
                        {
                            mask |= 1U << i;
                        }
                    }
                    const auto& cell = regularCellData[regularCellClass[mask]];
                    const auto base = static_cast<std::uint32_t>(mesh->vertices.size());
                    for (long v = 0; v < cell.GetVertexCount(); ++v)
                    {
                        const auto edge = regularVertexData[mask][v];
                        append_vertex(*mesh, vertex_layers, points[(edge >> 4U) & 15U],
                                      points[edge & 15U], spacing);
                    }
                    for (long triangle = 0; triangle < cell.GetTriangleCount() * 3; triangle += 3)
                    {
                        append_triangle(*mesh, base + cell.vertexIndex[triangle],
                                        base + cell.vertexIndex[triangle + 1],
                                        base + cell.vertexIndex[triangle + 2]);
                    }
                }
            }
        }
        std::shared_ptr<const schema::MeshDataT> collision;
        if (input.collision_required && input.transition_faces)
        {
            collision = std::make_shared<const schema::MeshDataT>(*mesh);
        }
        // Transition cells join the fine face contour to its two-sample coarse contour.
        // The endpoint ordering and 9-bit mask are defined by Lengyel's transition tables.
        constexpr std::array<unsigned, 9> mask_order{0, 1, 2, 5, 8, 7, 6, 3, 4};
        constexpr std::array<unsigned, 4> coarse_corners{0, 2, 6, 8};
        for (unsigned face = 0; face < 6; ++face)
        {
            if (!(input.transition_faces & (1U << face)))
            {
                continue;
            }
            const unsigned normal_axis = face / 2;
            const unsigned u_axis = (normal_axis + 1) % 3;
            const unsigned v_axis = (normal_axis + 2) % 3;
            for (int v = 0; v < cells; v += 2)
            {
                if (cancelled())
                {
                    return false;
                }
                for (int u = 0; u < cells; u += 2)
                {
                    std::array<Corner, 13> points;
                    for (unsigned i = 0; i < 9; ++i)
                    {
                        std::array<int, 3> point{};
                        point[normal_axis] = face % 2 ? cells : 0;
                        point[u_axis] = u + i % 3;
                        point[v_axis] = v + i / 3;
                        points[i] = corner(point[0], point[1], point[2]);
                    }
                    unsigned mask = 0;
                    for (unsigned i = 0; i < 9; ++i)
                    {
                        if (points[mask_order[i]].density < 0)
                        {
                            mask |= 1U << i;
                        }
                    }
                    for (unsigned i = 0; i < 4; ++i)
                    {
                        points[9 + i] = points[coarse_corners[i]];
                    }
                    const auto classification = transitionCellClass[mask];
                    const auto& cell = transitionCellData[classification & 127];
                    const auto base = static_cast<std::uint32_t>(mesh->vertices.size());
                    for (long i = 0; i < cell.GetVertexCount(); ++i)
                    {
                        const auto edge = transitionVertexData[mask][i];
                        append_vertex(*mesh, vertex_layers, points[(edge >> 4U) & 15U],
                                      points[edge & 15U], spacing);
                    }
                    for (long i = 0; i < cell.GetTriangleCount() * 3; i += 3)
                    {
                        auto b = base + cell.vertexIndex[i + 1];
                        auto c = base + cell.vertexIndex[i + 2];
                        if (bool(classification & 128) != bool(face % 2))
                        {
                            std::swap(b, c);
                        }
                        append_triangle(*mesh, base + cell.vertexIndex[i], b, c);
                    }
                }
            }
        }
        if (cancelled())
        {
            return false;
        }
        if (!source.material_layers.empty())
        {
            auto blended = std::make_shared<schema::MeshDataT>();
            blended->path = mesh->path;
            // Every triangle uses one constant palette. Only normalized weights interpolate.
            // At most four strongest layers contribute to one triangle.
            for (std::size_t triangle = 0; triangle < mesh->indices.size(); triangle += 3)
            {
                std::map<std::uint32_t, double> total;
                for (unsigned corner = 0; corner < 3; ++corner)
                {
                    for (auto [id, weight] : vertex_layers[mesh->indices[triangle + corner]])
                    {
                        total[id] += weight;
                    }
                }
                std::vector<std::pair<std::uint32_t, double>> palette(total.begin(), total.end());
                std::sort(
                    palette.begin(), palette.end(), [](const auto& a, const auto& b)
                    { return a.second != b.second ? a.second > b.second : a.first < b.first; });
                palette.resize(4);
                for (unsigned corner = 0; corner < 3; ++corner)
                {
                    const auto index = mesh->indices[triangle + corner];
                    std::array<double, 4> weights{};
                    double sum = 0;
                    for (unsigned i = 0; i < 4; ++i)
                    {
                        const auto found = vertex_layers[index].find(palette[i].first);
                        if (palette[i].second > 0 && found != vertex_layers[index].end())
                        {
                            weights[i] = found->second;
                        }
                        sum += weights[i];
                    }
                    if (sum <= 1e-12)
                    {
                        weights = {1, 0, 0, 0};
                        sum = 1;
                    }
                    blended->indices.push_back(std::uint32_t(blended->vertices.size()));
                    blended->vertices.push_back(mesh->vertices[index]);
                    blended->colors.emplace_back(weights[0] / sum, weights[1] / sum,
                                                 weights[2] / sum, weights[3] / sum);
                    blended->texcoords.emplace_back(palette[0].first + palette[1].first * 256U,
                                                    palette[2].first + palette[3].first * 256U);
                }
            }
            mesh = std::move(blended);
        }
        if (!mesh->indices.empty())
        {
            mesh->submeshes.emplace_back(0, std::uint32_t(mesh->indices.size()), 0);
        }
        output.key = input.key;
        output.source_revision = input.samples.source_revision;
        output.local_bounds = terrain_detail::region_bounds(source, input.key);
        if (input.collision_required)
        {
            output.collision.parts.push_back(collision ? collision : mesh);
        }
        output.mesh = std::move(mesh);
        return true;
    }
};
} // namespace
std::unique_ptr<TerrainMesher> create_terrain_mesher(std::string& error)
{
    error.clear();
    return std::make_unique<TransvoxelMesher>();
}
} // namespace engine::plugins::terrain
