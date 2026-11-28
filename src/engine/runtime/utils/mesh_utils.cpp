#include "runtime/utils/mesh_utils.hpp"
#include "runtime/performance/profiling.hpp"
#include "runtime/resource/mesh_storage.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace engine
{
namespace
{

using Vector = std::array<double, 3>;

bool float_value(double value) noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return std::isfinite(value) && std::abs(value) <= std::numeric_limits<float>::max();
}

bool unit_value(double value) noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return std::isfinite(value) && value >= 0.0F && value <= 1.0F;
}

double dot(const Vector& a, const Vector& b) noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

Vector cross(const Vector& a, const Vector& b) noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

bool normalize(Vector& value) noexcept
{
    ENGINE_PROFILE_FUNCTION();
    const double length = std::hypot(value[0], value[1], value[2]);
    if (!std::isfinite(length) || length <= 0.0)
    {
        return false;
    }
    for (double& item : value)
    {
        item /= length;
    }
    return true;
}

Vector orthogonal(const Vector& normal) noexcept
{
    ENGINE_PROFILE_FUNCTION();
    const Vector axis = std::abs(normal[2]) < 0.9 ? Vector{0.0, 0.0, 1.0} : Vector{0.0, 1.0, 0.0};
    Vector tangent = cross(axis, normal);
    const bool normalized = normalize(tangent);
    (void)normalized;
    return tangent;
}

bool validate_meshlets(const schema::MeshletDataT* data,
                       const std::vector<schema::MeshVertex>& vertices,
                       mesh_storage::MeshIndexView indices,
                       const std::vector<schema::MeshSubmesh>& submeshes)
{
    ENGINE_PROFILE_FUNCTION();
    if (data == nullptr)
    {
        return true;
    }
    const auto topology = mesh_storage::meshlet_indices(*data, indices);
    if (topology.size() != indices.size() || data->meshlets.empty() ||
        data->submeshes.size() != std::max<std::size_t>(1, submeshes.size()))
    {
        return false;
    }
    std::size_t next_index = 0;
    for (const auto& cluster : data->meshlets)
    {
        const auto& center = cluster.center();
        const auto& axis = cluster.cone_axis();
        const double axis_length = std::hypot(axis.x(), axis.y(), axis.z());
        if (cluster.first_index() != next_index || cluster.index_count() == 0 ||
            cluster.index_count() > 124 * 3 || cluster.index_count() % 3 != 0 ||
            next_index > topology.size() || cluster.index_count() > topology.size() - next_index ||
            !float_value(center.x()) || !float_value(center.y()) || !float_value(center.z()) ||
            !float_value(cluster.radius()) || cluster.radius() < 0 || !float_value(axis.x()) ||
            !float_value(axis.y()) || !float_value(axis.z()) ||
            !unit_value(cluster.cone_cutoff()) ||
            (cluster.cone_cutoff() < 1 && std::abs(axis_length - 1.0) > 0.001))
        {
            return false;
        }
        std::array<std::uint32_t, 64> unique{};
        std::size_t unique_count = 0;
        for (std::size_t j = 0; j < cluster.index_count(); ++j)
        {
            const auto index = topology[next_index + j];
            if (index >= vertices.size())
            {
                return false;
            }
            const auto end = unique.begin() + static_cast<std::ptrdiff_t>(unique_count);
            if (std::find(unique.begin(), end, index) == end)
            {
                if (unique_count == unique.size())
                {
                    return false;
                }
                unique[unique_count++] = index;
                const auto& vertex = vertices[index];
                const double distance = std::hypot(vertex.x() - center.x(), vertex.y() - center.y(),
                                                   vertex.z() - center.z());
                if (distance > cluster.radius() + std::max(1.0, distance) * 1.0e-5)
                {
                    return false;
                }
            }
        }
        next_index += cluster.index_count();
    }
    if (next_index != indices.size())
    {
        return false;
    }
    // Exact per-material multiset: preserve winding, provoking vertex and duplicate triangles.
    using Triangle = std::array<std::uint32_t, 3>;
    std::vector<Triangle> original;
    std::vector<Triangle> generated;
    std::size_t next_meshlet = 0;
    for (std::size_t part = 0; part < data->submeshes.size(); ++part)
    {
        const auto& range = data->submeshes[part];
        if (range.first_meshlet() != next_meshlet || range.meshlet_count() == 0 ||
            next_meshlet > data->meshlets.size() ||
            range.meshlet_count() > data->meshlets.size() - next_meshlet)
        {
            return false;
        }
        const auto first = submeshes.empty() ? 0 : submeshes[part].first_index();
        const auto count = submeshes.empty() ? indices.size() : submeshes[part].index_count();
        // Shared topology is already identical when this material covers the same interval.
        const auto& first_cluster = data->meshlets[next_meshlet];
        const auto& last_cluster = data->meshlets[next_meshlet + range.meshlet_count() - 1];
        if (data->uses_parent_indices && first_cluster.first_index() == first &&
            static_cast<std::size_t>(last_cluster.first_index()) + last_cluster.index_count() ==
                static_cast<std::size_t>(first) + count)
        {
            next_meshlet += range.meshlet_count();
            continue;
        }
        original.clear();
        generated.clear();
        for (std::size_t i = first; i < first + count; i += 3)
        {
            original.push_back({indices[i], indices[i + 1], indices[i + 2]});
        }
        for (std::size_t i = next_meshlet; i < next_meshlet + range.meshlet_count(); ++i)
        {
            const auto& cluster = data->meshlets[i];
            for (std::size_t j = cluster.first_index();
                 j < cluster.first_index() + cluster.index_count(); j += 3)
            {
                generated.push_back({topology[j], topology[j + 1], topology[j + 2]});
            }
        }
        std::sort(original.begin(), original.end());
        std::sort(generated.begin(), generated.end());
        if (original != generated)
        {
            return false;
        }
        next_meshlet += range.meshlet_count();
    }
    return next_meshlet == data->meshlets.size();
}

} // namespace

bool MeshUtils::validate(const schema::MeshDataT& data) noexcept
{
    ENGINE_PROFILE_FUNCTION();
    if (!mesh_storage::valid_encoding(data))
    {
        return false;
    }
    const auto indices = mesh_storage::indices(data);
    const auto uvs = mesh_storage::texcoord_count(data);
    const auto lightmap_uvs = mesh_storage::lightmap_texcoord_count(data);
    const auto tangents = mesh_storage::tangent_count(data);
    const auto count = data.vertices.size();
    if (data.schema_version != 2 || count == 0 ||
        count > std::numeric_limits<std::uint32_t>::max() / (16 * sizeof(float)) ||
        indices.empty() || indices.size() % 3 != 0 ||
        indices.size() > std::numeric_limits<std::uint32_t>::max() / sizeof(std::uint32_t) ||
        (uvs != 0 && uvs != count) || (lightmap_uvs != 0 && lightmap_uvs != count) ||
        (tangents != 0 && tangents != count) ||
        (!data.colors.empty() && data.colors.size() != count))
    {
        return false;
    }
    for (const auto& vertex : data.vertices)
    {
        if (!float_value(vertex.x()) || !float_value(vertex.y()) || !float_value(vertex.z()) ||
            !float_value(vertex.nx()) || !float_value(vertex.ny()) || !float_value(vertex.nz()) ||
            std::hypot(static_cast<double>(vertex.nx()), static_cast<double>(vertex.ny()),
                       static_cast<double>(vertex.nz())) <= 0.0)
        {
            return false;
        }
    }
    for (std::size_t i = 0; i < indices.size(); ++i)
    {
        if (indices[i] >= count)
        {
            return false;
        }
    }
    for (std::size_t i = 0; i < uvs; ++i)
    {
        const auto uv = mesh_storage::texcoord(data, i);
        if (!float_value(uv.x()) || !float_value(uv.y()))
        {
            return false;
        }
    }
    for (std::size_t i = 0; i < lightmap_uvs; ++i)
    {
        const auto uv = mesh_storage::lightmap_texcoord(data, i);
        if (!unit_value(uv.x()) || !unit_value(uv.y()))
        {
            return false;
        }
    }
    for (std::size_t i = 0; i < tangents; ++i)
    {
        const auto tangent = mesh_storage::tangent(data, i);
        if (!float_value(tangent.x()) || !float_value(tangent.y()) || !float_value(tangent.z()) ||
            (tangent.w() != -1.0 && tangent.w() != 1.0) ||
            std::hypot(tangent.x(), tangent.y(), tangent.z()) <= 0.0)
        {
            return false;
        }
    }
    for (const auto& color : data.colors)
    {
        if (!unit_value(color.r()) || !unit_value(color.g()) || !unit_value(color.b()) ||
            !unit_value(color.a()))
        {
            return false;
        }
    }
    std::size_t next_index = 0;
    for (const auto& submesh : data.submeshes)
    {
        if (submesh.first_index() != next_index || submesh.index_count() == 0 ||
            submesh.index_count() % 3 != 0 || submesh.material_slot() >= 16 ||
            submesh.index_count() > indices.size() - next_index)
        {
            return false;
        }
        next_index += submesh.index_count();
    }
    if (!data.submeshes.empty() && next_index != indices.size())
    {
        return false;
    }
    if ((!data.lods.empty() || data.meshlets) &&
        (!data.skin.empty() || !data.skeleton_guid.empty()))
    {
        return false;
    }
    std::size_t previous_count = indices.size();
    float previous_height = std::nextafter(1.0F, 2.0F);
    for (const auto& lod : data.lods)
    {
        if (!lod)
        {
            return false;
        }
        const auto lod_indices = mesh_storage::indices(*lod);
        if (!std::isfinite(lod->screen_height) || lod->screen_height <= 0 ||
            lod->screen_height > 1 || lod->screen_height >= previous_height ||
            lod_indices.empty() || lod_indices.size() % 3 != 0 ||
            lod_indices.size() >= previous_count ||
            static_cast<bool>(lod->meshlets) != static_cast<bool>(data.meshlets))
        {
            return false;
        }
        for (std::size_t i = 0; i < lod_indices.size(); ++i)
        {
            if (lod_indices[i] >= count)
            {
                return false;
            }
        }
        if (lod->submeshes.size() != std::max<std::size_t>(1, data.submeshes.size()))
        {
            return false;
        }
        next_index = 0;
        for (std::size_t part = 0; part < lod->submeshes.size(); ++part)
        {
            const auto& range = lod->submeshes[part];
            const auto slot = data.submeshes.empty() ? 0U : data.submeshes[part].material_slot();
            if (range.first_index() != next_index || range.index_count() == 0 ||
                range.index_count() % 3 != 0 || range.material_slot() != slot ||
                next_index > lod_indices.size() ||
                range.index_count() > lod_indices.size() - next_index)
            {
                return false;
            }
            next_index += range.index_count();
        }
        if (next_index != lod_indices.size() ||
            !validate_meshlets(lod->meshlets.get(), data.vertices, lod_indices, lod->submeshes))
        {
            return false;
        }
        previous_count = lod_indices.size();
        previous_height = lod->screen_height;
    }
    return validate_meshlets(data.meshlets.get(), data.vertices, indices, data.submeshes);
}

bool MeshUtils::build_tangents(const schema::MeshDataT& data, std::vector<schema::Vec4>& output)
{
    ENGINE_PROFILE_FUNCTION();
    if (!validate(data))
    {
        output.clear();
        return false;
    }
    const bool supplied_tangents = mesh_storage::tangent_count(data) != 0;
    const auto indices = mesh_storage::indices(data);
    std::vector<Vector> tangents;
    std::vector<Vector> bitangents;
    // Adapted from bgfx examples/common/bgfx_utils.cpp calcTangents (BSD-2-Clause).
    // Copyright 2010-2025 Branimir Karadzic. See third_party/notices/bgfx.txt.
    // Changes: generated data, uint32 indices, double arithmetic and degenerate-UV fallback.
    if (!supplied_tangents)
    {
        tangents.resize(data.vertices.size());
        bitangents.resize(data.vertices.size());
    }
    if (!supplied_tangents && mesh_storage::texcoord_count(data) != 0)
    {
        for (std::size_t triangle = 0; triangle < indices.size(); triangle += 3)
        {
            const auto i0 = indices[triangle];
            const auto i1 = indices[triangle + 1];
            const auto i2 = indices[triangle + 2];
            const auto& a = data.vertices[i0];
            const auto& b = data.vertices[i1];
            const auto& c = data.vertices[i2];
            const Vector edge_b = {static_cast<double>(b.x()) - a.x(),
                                   static_cast<double>(b.y()) - a.y(),
                                   static_cast<double>(b.z()) - a.z()};
            const Vector edge_c = {static_cast<double>(c.x()) - a.x(),
                                   static_cast<double>(c.y()) - a.y(),
                                   static_cast<double>(c.z()) - a.z()};
            const auto uv0 = mesh_storage::texcoord(data, i0);
            const auto uv1 = mesh_storage::texcoord(data, i1);
            const auto uv2 = mesh_storage::texcoord(data, i2);
            const double bu = uv1.x() - uv0.x();
            const double bv = uv1.y() - uv0.y();
            const double cu = uv2.x() - uv0.x();
            const double cv = uv2.y() - uv0.y();
            const double determinant = bu * cv - bv * cu;
            if (std::abs(determinant) <= 1.0e-20)
            {
                continue;
            }
            for (std::size_t axis = 0; axis < 3; ++axis)
            {
                const double tangent = (edge_b[axis] * cv - edge_c[axis] * bv) / determinant;
                const double bitangent = (edge_c[axis] * bu - edge_b[axis] * cu) / determinant;
                for (auto index : {i0, i1, i2})
                {
                    tangents[index][axis] += tangent;
                    bitangents[index][axis] += bitangent;
                }
            }
        }
    }
    std::vector<schema::Vec4> result;
    result.reserve(data.vertices.size());
    for (std::size_t index = 0; index < data.vertices.size(); ++index)
    {
        const auto& vertex = data.vertices[index];
        Vector normal = {vertex.nx(), vertex.ny(), vertex.nz()};
        if (!normalize(normal))
        {
            output.clear();
            return false;
        }
        Vector tangent{};
        double handedness = 1.0;
        if (supplied_tangents)
        {
            const auto supplied = mesh_storage::tangent(data, index);
            tangent = {supplied.x(), supplied.y(), supplied.z()};
            handedness = supplied.w();
        }
        else
        {
            tangent = tangents[index];
        }
        const double normal_dot_tangent = dot(normal, tangent);
        for (std::size_t axis = 0; axis < 3; ++axis)
        {
            tangent[axis] -= normal[axis] * normal_dot_tangent;
        }
        if (!normalize(tangent))
        {
            tangent = orthogonal(normal);
        }
        else if (!supplied_tangents)
        {
            handedness = dot(cross(normal, tangent), bitangents[index]) < 0.0 ? -1.0 : 1.0;
        }
        result.emplace_back(tangent[0], tangent[1], tangent[2], handedness);
    }
    output = std::move(result);
    return true;
}

} // namespace engine
