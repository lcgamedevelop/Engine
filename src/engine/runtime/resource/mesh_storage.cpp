#include "runtime/resource/mesh_storage.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace engine::mesh_storage
{
namespace
{
// A stream uses exactly one width. Omitted optional attributes have neither.
template <typename Wide, typename Narrow>
bool unambiguous(const std::vector<Wide>& wide, const std::vector<Narrow>& narrow) noexcept
{
    return wide.empty() || narrow.empty();
}

template <typename Level> bool valid_level(const Level& level) noexcept
{
    if (!unambiguous(level.indices, level.indices16))
    {
        return false;
    }
    if (!level.meshlets)
    {
        return true;
    }
    const auto& clusters = *level.meshlets;
    if (clusters.uses_parent_indices)
    {
        return clusters.indices.empty() && clusters.indices16.empty() && !indices(level).empty();
    }
    return unambiguous(clusters.indices, clusters.indices16);
}

template <typename Level> bool compact_level_present(const Level& level) noexcept
{
    return !level.indices16.empty() || (level.meshlets && (level.meshlets->uses_parent_indices ||
                                                           !level.meshlets->indices16.empty()));
}

bool exact_float(double value) noexcept
{
    constexpr double limit = std::numeric_limits<float>::max();
    if (!std::isfinite(value) || value < -limit || value > limit)
    {
        return false;
    }
    const double restored = static_cast<float>(value);
    return std::bit_cast<std::uint64_t>(restored) == std::bit_cast<std::uint64_t>(value);
}

bool fits(const schema::Vec2& value) noexcept
{
    return exact_float(value.x()) && exact_float(value.y());
}

bool fits(const schema::Vec4& value) noexcept
{
    return exact_float(value.x()) && exact_float(value.y()) && exact_float(value.z()) &&
           exact_float(value.w());
}

schema::MeshFloat2 narrow_value(const schema::Vec2& v) noexcept
{
    return {static_cast<float>(v.x()), static_cast<float>(v.y())};
}

schema::MeshFloat4 narrow_value(const schema::Vec4& v) noexcept
{
    return {static_cast<float>(v.x()), static_cast<float>(v.y()), static_cast<float>(v.z()),
            static_cast<float>(v.w())};
}

schema::Vec2 wide_value(const schema::MeshFloat2& v) noexcept
{
    return {v.x(), v.y()};
}

schema::Vec4 wide_value(const schema::MeshFloat4& v) noexcept
{
    return {v.x(), v.y(), v.z(), v.w()};
}

template <typename Wide, typename Narrow>
void compact_attribute(std::vector<Wide>& values, std::vector<Narrow>& narrow)
{
    if (values.empty() ||
        !std::all_of(values.begin(), values.end(), [](const Wide& value) { return fits(value); }))
    {
        return;
    }
    narrow.reserve(values.size());
    for (const auto& value : values)
    {
        narrow.push_back(narrow_value(value));
    }
    std::vector<Wide>().swap(values);
}

template <typename Wide, typename Narrow>
void expand_attribute(std::vector<Wide>& values, std::vector<Narrow>& narrow)
{
    if (narrow.empty())
    {
        return;
    }
    values.reserve(narrow.size());
    for (const auto& value : narrow)
    {
        values.push_back(wide_value(value));
    }
    std::vector<Narrow>().swap(narrow);
}

template <typename T> void compact_indices(T& stream)
{
    constexpr auto max = std::numeric_limits<std::uint16_t>::max();
    if (!stream.indices.empty() && std::ranges::max(stream.indices) <= max)
    {
        stream.indices16.assign(stream.indices.begin(), stream.indices.end());
        std::vector<std::uint32_t>().swap(stream.indices);
    }
}

template <typename T> void expand_indices(T& stream)
{
    if (!stream.indices16.empty())
    {
        stream.indices.assign(stream.indices16.begin(), stream.indices16.end());
        std::vector<std::uint16_t>().swap(stream.indices16);
    }
}

bool equal_indices(MeshIndexView first, MeshIndexView second)
{
    if (first.size() != second.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < first.size(); ++i)
    {
        if (first[i] != second[i])
        {
            return false;
        }
    }
    return true;
}

template <typename Level> void compact_level(Level& level)
{
    if (auto* clusters = level.meshlets.get(); clusters && !clusters->uses_parent_indices)
    {
        const auto regular = indices(level);
        if (!regular.empty() && equal_indices(regular, meshlet_indices(*clusters, regular)))
        {
            std::vector<std::uint32_t>().swap(clusters->indices);
            std::vector<std::uint16_t>().swap(clusters->indices16);
            clusters->uses_parent_indices = true;
        }
        else
        {
            compact_indices(*clusters);
        }
    }
    compact_indices(level);
}

template <typename Level> void expand_level(Level& level)
{
    expand_indices(level);
    if (auto* clusters = level.meshlets.get())
    {
        if (clusters->uses_parent_indices)
        {
            clusters->indices = level.indices;
            clusters->uses_parent_indices = false;
        }
        else
        {
            expand_indices(*clusters);
        }
    }
}
} // namespace

void MeshIndexView::copy_to(std::vector<std::uint32_t>& output) const
{
    if (!narrow.empty())
    {
        output.assign(narrow.begin(), narrow.end());
    }
    else if (wide.data() != output.data() || wide.size() != output.size())
    {
        output.assign(wide.begin(), wide.end());
    }
}

bool is_compact(const schema::MeshDataT& mesh) noexcept
{
    return !mesh.texcoords32.empty() || !mesh.tangents32.empty() ||
           !mesh.lightmap_texcoords32.empty() || compact_level_present(mesh) ||
           std::ranges::any_of(mesh.lods,
                               [](const auto& lod) { return lod && compact_level_present(*lod); });
}

bool valid_encoding(const schema::MeshDataT& mesh) noexcept
{
    return unambiguous(mesh.texcoords, mesh.texcoords32) &&
           unambiguous(mesh.tangents, mesh.tangents32) &&
           unambiguous(mesh.lightmap_texcoords, mesh.lightmap_texcoords32) && valid_level(mesh) &&
           std::ranges::all_of(mesh.lods, [](const auto& lod) { return lod && valid_level(*lod); });
}

bool compact(schema::MeshDataT& mesh)
{
    if (!valid_encoding(mesh))
    {
        return false;
    }
    compact_attribute(mesh.texcoords, mesh.texcoords32);
    compact_attribute(mesh.tangents, mesh.tangents32);
    compact_attribute(mesh.lightmap_texcoords, mesh.lightmap_texcoords32);
    compact_level(mesh);
    for (auto& lod : mesh.lods)
    {
        compact_level(*lod);
    }
    return true;
}

bool expand(schema::MeshDataT& mesh)
{
    if (!valid_encoding(mesh))
    {
        return false;
    }
    expand_attribute(mesh.texcoords, mesh.texcoords32);
    expand_attribute(mesh.tangents, mesh.tangents32);
    expand_attribute(mesh.lightmap_texcoords, mesh.lightmap_texcoords32);
    expand_level(mesh);
    for (auto& lod : mesh.lods)
    {
        expand_level(*lod);
    }
    return true;
}
} // namespace engine::mesh_storage
