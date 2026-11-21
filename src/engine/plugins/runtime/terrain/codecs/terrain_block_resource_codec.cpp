#include "plugins/runtime/terrain/codecs/terrain_block_resource_codec.hpp"
#include "runtime/performance/function_profile.hpp"
#include "runtime/performance/profiling.hpp"

#include "plugins/runtime/terrain/terrain_samples.hpp"

#include <flatbuffers/verifier.h>

#include <algorithm>
#include <cmath>

namespace engine::plugins::terrain
{
schema::AssetKind TerrainBlockResourceCodec::kind() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return schema::AssetKind::terrain_block;
}

bool TerrainBlockResourceCodec::decode(const schema::AssetEntryT& entry, ByteView payload,
                                       std::unique_ptr<Resource>& output, std::string& error) const
{
    ENGINE_PROFILE_FUNCTION();
    error.clear();
    if (output || entry.kind != kind() || payload.empty() || payload.size() > 4U * 1024U * 1024U)
    {
        error = "Invalid terrain block resource or payload size";
        return false;
    }
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(payload.data());
    flatbuffers::Verifier verifier(bytes, payload.size());
    if (!schema::VerifyTerrainBlockDataBuffer(verifier))
    {
        error = "Invalid terrain block FlatBuffer";
        return false;
    }
    auto data = schema::UnPackTerrainBlockData(bytes);
    output = std::make_unique<ResourceType>(entry, std::move(*data));
    return true;
}

bool TerrainBlockResourceCodec::validate_data(const Resource& resource, std::string& error) const
{
    ENGINE_PROFILE_FUNCTION();
    error.clear();
    const auto* data = resource_detail::data<ResourceType>(&resource);
    if (!data || data->schema_version != 2 || data->key.lod() > 15 ||
        !resource.info().dependencies.empty() || !resource.info().stream_dependencies.empty() ||
        (data->uniform ? data->density.size() != 1
                       : data->density.size() != 512 && data->density.size() != 4096 &&
                             data->density.size() != 32768) ||
        data->layers.size() != data->density.size() ||
        !std::all_of(data->density.begin(), data->density.end(),
                     [](float v)
                     {
                         ENGINE_PROFILE_FUNCTION_PROBE();
                         return std::isfinite(v);
                     }) ||
        !std::all_of(data->layers.begin(), data->layers.end(),
                     [](const auto& layer)
                     {
                         ENGINE_PROFILE_FUNCTION_PROBE();
                         return terrain_detail::layer_valid(layer, 256);
                     }))
    {
        error = "Invalid terrain block dimensions, samples, layers or dependencies";
        return false;
    }
    return true;
}

bool TerrainBlockResourceCodec::validate_dependencies(const Resource& resource,
                                                      ResourceView dependencies,
                                                      std::string& error) const
{
    ENGINE_PROFILE_FUNCTION();
    error.clear();
    if (!resource.info().dependencies.empty() || !dependencies.empty())
    {
        error = "Terrain block resources cannot own eager dependencies";
        return false;
    }
    return true;
}
} // namespace engine::plugins::terrain
