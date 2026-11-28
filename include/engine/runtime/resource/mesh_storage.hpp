#pragma once

#include "runtime/schema/mesh_generated.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace engine::mesh_storage
{
// Borrows one stream; a meshlet can point into its parent topology.
struct MeshIndexView
{
    std::span<const std::uint32_t> wide;
    std::span<const std::uint16_t> narrow;

    [[nodiscard]] std::size_t size() const noexcept
    {
        return narrow.empty() ? wide.size() : narrow.size();
    }
    [[nodiscard]] bool empty() const noexcept
    {
        return wide.empty() && narrow.empty();
    }
    [[nodiscard]] std::uint32_t operator[](std::size_t index) const noexcept
    {
        return narrow.empty() ? wide[index] : narrow[index];
    }
    void copy_to(std::vector<std::uint32_t>& output) const;
};

[[nodiscard]] inline MeshIndexView indices(const schema::MeshDataT& mesh) noexcept
{
    return {mesh.indices, mesh.indices16};
}

[[nodiscard]] inline MeshIndexView indices(const schema::MeshLodDataT& lod) noexcept
{
    return {lod.indices, lod.indices16};
}

[[nodiscard]] inline MeshIndexView meshlet_indices(const schema::MeshletDataT& meshlets,
                                                   MeshIndexView parent) noexcept
{
    return meshlets.uses_parent_indices ? parent
                                        : MeshIndexView{meshlets.indices, meshlets.indices16};
}

[[nodiscard]] inline std::size_t texcoord_count(const schema::MeshDataT& mesh) noexcept
{
    return mesh.texcoords32.empty() ? mesh.texcoords.size() : mesh.texcoords32.size();
}

[[nodiscard]] inline std::size_t lightmap_texcoord_count(const schema::MeshDataT& mesh) noexcept
{
    return mesh.lightmap_texcoords32.empty() ? mesh.lightmap_texcoords.size()
                                             : mesh.lightmap_texcoords32.size();
}

[[nodiscard]] inline std::size_t tangent_count(const schema::MeshDataT& mesh) noexcept
{
    return mesh.tangents32.empty() ? mesh.tangents.size() : mesh.tangents32.size();
}

[[nodiscard]] inline schema::Vec2 texcoord(const schema::MeshDataT& mesh,
                                           std::size_t index) noexcept
{
    if (mesh.texcoords32.empty())
    {
        return mesh.texcoords[index];
    }
    const auto& uv = mesh.texcoords32[index];
    return {uv.x(), uv.y()};
}

[[nodiscard]] inline schema::Vec2 lightmap_texcoord(const schema::MeshDataT& mesh,
                                                    std::size_t index) noexcept
{
    if (mesh.lightmap_texcoords32.empty())
    {
        return mesh.lightmap_texcoords[index];
    }
    const auto& uv = mesh.lightmap_texcoords32[index];
    return {uv.x(), uv.y()};
}

[[nodiscard]] inline schema::Vec4 tangent(const schema::MeshDataT& mesh, std::size_t index) noexcept
{
    if (mesh.tangents32.empty())
    {
        return mesh.tangents[index];
    }
    const auto& value = mesh.tangents32[index];
    return {value.x(), value.y(), value.z(), value.w()};
}

[[nodiscard]] bool is_compact(const schema::MeshDataT& mesh) noexcept;
[[nodiscard]] bool valid_encoding(const schema::MeshDataT& mesh) noexcept;
// Both operations reject ambiguous streams without changing the input.
[[nodiscard]] bool compact(schema::MeshDataT& mesh);
[[nodiscard]] bool expand(schema::MeshDataT& mesh);
} // namespace engine::mesh_storage
