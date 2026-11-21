#include "runtime/resource/codecs/source_resource_codec.hpp"
#include "runtime/performance/profiling.hpp"

namespace engine
{
schema::AssetKind SourceResourceCodec::kind() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return schema::AssetKind::source;
}
bool SourceResourceCodec::decode(const schema::AssetEntryT& entry, ByteView payload,
                                 std::unique_ptr<Resource>& output, std::string& error) const
{
    ENGINE_PROFILE_FUNCTION();
    error.clear();
    if (output || entry.kind != kind() || payload.size() > 64U * 1024U * 1024U)
    {
        error = "Invalid resource output, kind or payload size";
        return false;
    }
    output = std::make_unique<ResourceType>(entry, ByteBuffer(payload.begin(), payload.end()));
    return true;
}
bool SourceResourceCodec::validate_data(const Resource& resource, std::string& error) const
{
    ENGINE_PROFILE_FUNCTION();
    error.clear();
    const auto* data = resource_detail::data<ResourceType>(&resource);
    if (!data || data->size() > 64U * 1024U * 1024U || !(resource.info().dependencies.empty()))
    {
        error = "Invalid source resource";
        return false;
    }
    return true;
}
bool SourceResourceCodec::validate_dependencies(const Resource& resource, ResourceView dependencies,
                                                std::string& error) const
{
    ENGINE_PROFILE_FUNCTION();
    error.clear();
    return resource.info().dependencies.empty() && dependencies.empty();
}
} // namespace engine
