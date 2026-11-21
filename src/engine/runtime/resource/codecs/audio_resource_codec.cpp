#include "runtime/resource/codecs/audio_resource_codec.hpp"
#include "runtime/performance/function_profile.hpp"
#include "runtime/performance/profiling.hpp"
#include "runtime/resource/codecs/resource_decode.hpp"

#include <set>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace engine
{
namespace
{
constexpr std::size_t maximum_samples = (64U * 1024U * 1024U - 128U * 1024U) / sizeof(float);
}

bool AudioResourceCodec::validate(const schema::AudioClipDataT& data) noexcept
{
    ENGINE_PROFILE_FUNCTION();
    if (data.schema_version != 1 || data.sample_rate < 8000 || data.sample_rate > 192000 ||
        (data.channels != 1 && data.channels != 2) || data.frames == 0 ||
        data.samples.size() > maximum_samples || data.frames > maximum_samples / data.channels ||
        data.samples.size() != data.frames * data.channels)
    {
        return false;
    }
    return std::all_of(data.samples.begin(), data.samples.end(), [](float value)
                       {
        ENGINE_PROFILE_FUNCTION_PROBE();
        return std::isfinite(value) && value >= -1.0F && value <= 1.0F;
    });
}

schema::AssetKind AudioResourceCodec::kind() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return schema::AssetKind::audio;
}

bool AudioResourceCodec::decode(const schema::AssetEntryT& entry, ByteView payload,
                                std::unique_ptr<Resource>& output, std::string& error) const
{
    ENGINE_PROFILE_FUNCTION();
    const auto validate = [](ByteView payload, std::string& error)
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        if (!resource_decode_detail::verify_flatbuffer(payload, schema::VerifyAudioClipDataBuffer,
                                                       "Invalid Audio FlatBuffer", error))
        {
            return false;
        }
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(payload.data());
        if (schema::GetAudioClipData(bytes)->samples()->size() > maximum_samples)
        {
            error = "Audio sample limit exceeded";
            return false;
        }
        return true;
    };
    return resource_decode_detail::decode_flatbuffer<ResourceType>(
        entry, payload, 64U * 1024U * 1024U, validate, [](const std::uint8_t* bytes)
        {
            ENGINE_PROFILE_FUNCTION_PROBE();
            return schema::UnPackAudioClipData(bytes);
        }, output, error);
}

bool AudioResourceCodec::validate_data(const Resource& resource, std::string& error) const
{
    ENGINE_PROFILE_FUNCTION();
    const auto* data = resource_detail::data<ResourceType>(&resource);
    if (!data ||
        !(data->schema_version == 1 && resource.info().dependencies.empty() && validate(*data)))
    {
        error = "Invalid Audio data or declared dependencies";
        return false;
    }
    error.clear();
    return true;
}

bool AudioResourceCodec::validate_dependencies(const Resource& resource, ResourceView dependencies,
                                               std::string& error) const
{
    ENGINE_PROFILE_FUNCTION();
    error.clear();
    return resource.info().dependencies.empty() && dependencies.empty();
}
} // namespace engine
