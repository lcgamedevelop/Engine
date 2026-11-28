#include "runtime/resource/codecs/mesh_resource_codec.hpp"
#include "runtime/performance/function_profile.hpp"
#include "runtime/performance/profiling.hpp"
#include "runtime/resource/codecs/resource_decode.hpp"

#include "runtime/core/guid_map.hpp"
#include "runtime/resource/codecs/model_value_utils.hpp"
#include "runtime/resource/mesh_storage.hpp"
#include "runtime/resource/types/skeleton_resource.hpp"
#include "runtime/utils/mesh_utils.hpp"

#include <utility>

namespace engine
{
std::uint64_t resource_detail::Payload<schema::AssetKind::mesh>::cpu_payload_capacity(
    const schema::MeshDataT& mesh) noexcept
{
    ENGINE_PROFILE_FUNCTION_PROBE();
    const auto bytes = []<typename T>(const std::vector<T>& values) -> std::uint64_t
    { return static_cast<std::uint64_t>(values.capacity()) * sizeof(T); };
    const auto meshlet_bytes =
        [&](const std::unique_ptr<schema::MeshletDataT>& data) -> std::uint64_t
    {
        return data ? bytes(data->indices) + bytes(data->indices16) + bytes(data->meshlets) +
                          bytes(data->submeshes)
                    : 0;
    };
    auto total = bytes(mesh.vertices) + bytes(mesh.indices) + bytes(mesh.texcoords) +
                 bytes(mesh.tangents) + bytes(mesh.colors) + bytes(mesh.submeshes) +
                 bytes(mesh.skin) + bytes(mesh.mesh_to_skeleton) + bytes(mesh.lightmap_texcoords) +
                 bytes(mesh.lods) + meshlet_bytes(mesh.meshlets) + bytes(mesh.indices16) +
                 bytes(mesh.texcoords32) + bytes(mesh.tangents32) +
                 bytes(mesh.lightmap_texcoords32);
    for (const auto& lod : mesh.lods)
    {
        if (lod)
        {
            total += bytes(lod->indices) + bytes(lod->indices16) + bytes(lod->submeshes) +
                     meshlet_bytes(lod->meshlets);
        }
    }
    return total;
}

bool MeshResourceCodec::validate(const schema::MeshDataT& data)
{
    ENGINE_PROFILE_FUNCTION();
    if (!valid_path(data.path) || !MeshUtils::validate(data))
    {
        return false;
    }
    if (data.skeleton_guid.empty())
    {
        return data.skin.empty() && data.mesh_to_skeleton.empty();
    }
    if (!GuidMap::valid_guid(data.skeleton_guid) || data.skin.size() != data.vertices.size() ||
        !model_value_utils::affine(data.mesh_to_skeleton))
    {
        return false;
    }
    for (const auto& skin : data.skin)
    {
        const float weights[] = {skin.w0(), skin.w1(), skin.w2(), skin.w3()};
        double sum = 0;
        for (float weight : weights)
        {
            if (!std::isfinite(weight) || weight < 0 || weight > 1)
            {
                return false;
            }
            sum += weight;
        }
        if (std::abs(sum - 1) > 0.0001 || skin.j0() >= 1024 || skin.j1() >= 1024 ||
            skin.j2() >= 1024 || skin.j3() >= 1024)
        {
            return false;
        }
    }
    return true;
}

schema::AssetKind MeshResourceCodec::kind() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return schema::AssetKind::mesh;
}

bool MeshResourceCodec::decode(const schema::AssetEntryT& entry, ByteView payload,
                               std::unique_ptr<Resource>& output, std::string& error) const
{
    ENGINE_PROFILE_FUNCTION();
    if (output)
    {
        error = "Resource output must be empty";
        return false;
    }
    if (entry.kind != ResourceType::asset_kind || payload.empty() ||
        payload.size() > 64U * 1024U * 1024U)
    {
        error = "Invalid resource kind or payload size";
        return false;
    }
    if (!resource_decode_detail::verify_flatbuffer(payload, schema::VerifyMeshDataBuffer,
                                                   "Invalid Mesh FlatBuffer", error))
    {
        return false;
    }
    auto mesh = schema::UnPackMeshData(reinterpret_cast<const std::uint8_t*>(payload.data()));
    const bool prepared =
        mesh->optimize_storage ? mesh_storage::compact(*mesh) : mesh_storage::expand(*mesh);
    if (!prepared)
    {
        error = "Conflicting mesh stream encodings";
        return false;
    }
    output = std::make_unique<MeshResource>(entry, std::move(*mesh));
    error.clear();
    return true;
}

bool MeshResourceCodec::validate_data(const Resource& resource, std::string& error) const
{
    ENGINE_PROFILE_FUNCTION();
    const auto* data = resource_detail::data<ResourceType>(&resource);
    if (!data || !(data->path == resource.info().path && validate(*data) &&
                   resource.info().dependencies.size() == (data->skeleton_guid.empty() ? 0U : 1U) &&
                   (data->skeleton_guid.empty() || declares(resource, data->skeleton_guid))))
    {
        error = "Invalid Mesh data or declared dependencies";
        return false;
    }
    error.clear();
    return true;
}

bool MeshResourceCodec::validate_dependencies(const Resource& resource, ResourceView dependencies,
                                              std::string&) const
{
    ENGINE_PROFILE_FUNCTION();
    const auto* data = resource_detail::data<MeshResource>(&resource);
    if (!data)
    {
        return false;
    }
    if (data->skeleton_guid.empty())
    {
        return true;
    }
    const auto* skeleton = dependency(dependencies, data->skeleton_guid);
    if (!skeleton || !resource_detail::data<SkeletonResource>(skeleton))
    {
        return false;
    }
    const auto count = resource_detail::data<SkeletonResource>(skeleton)->joints.size();
    for (const auto& skin : data->skin)
    {
        if (skin.j0() >= count || skin.j1() >= count || skin.j2() >= count || skin.j3() >= count)
        {
            return false;
        }
    }
    return true;
}

} // namespace engine
