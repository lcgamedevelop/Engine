#include "runtime/resource/resource_codec.hpp"
#include "runtime/performance/function_profile.hpp"
#include "runtime/performance/profiling.hpp"

#include "runtime/core/guid_map.hpp"

#include <algorithm>

namespace engine
{
bool ResourceCodec::valid_path(std::string_view path) noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return GuidMap::valid_path(path) && !path.starts_with("editor://");
}
bool ResourceCodec::declares(const Resource& resource, std::string_view guid) noexcept
{
    ENGINE_PROFILE_FUNCTION();
    const auto& declared = resource.info().dependencies;
    return GuidMap::valid_guid(guid) &&
           std::find(declared.begin(), declared.end(), guid) != declared.end();
}
const Resource* ResourceCodec::dependency(ResourceView resources,
                                          std::string_view path_or_guid) noexcept
{
    ENGINE_PROFILE_FUNCTION();
    const auto found = std::find_if(resources.begin(), resources.end(),
                                    [path_or_guid](const auto* value)
                                    {
                                        ENGINE_PROFILE_FUNCTION_PROBE();
                                        return value && (value->info().guid == path_or_guid ||
                                                         value->info().path == path_or_guid);
                                    });
    return found == resources.end() ? nullptr : *found;
}
} // namespace engine
