#include "editor/asset_pipeline/components/audio_listener_codec.hpp"
#include "runtime/performance/profiling.hpp"
#include "runtime/scene/audio_listener.hpp"
#include <array>
#include <utility>
namespace engine::editor::asset_pipeline
{
schema::ComponentPayload AudioListenerCodec::type() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return schema::ComponentPayload::AudioListenerData;
}
std::string_view AudioListenerCodec::name() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return "audio_listener";
}
bool AudioListenerCodec::validate(const schema::ComponentDataT& component, std::string& error) const
{
    ENGINE_PROFILE_FUNCTION();
    const auto* value = component.payload.AsAudioListenerData();
    if (value == nullptr || !engine::AudioListener::valid_settings(*value))
    {
        error = "invalid audio_listener settings";
        return false;
    }
    return true;
}
bool AudioListenerCodec::read(const toml::table& table, schema::ComponentDataT& component,
                              std::string& error) const
{
    ENGINE_PROFILE_FUNCTION();
    schema::AudioListenerDataT value;
    if (table.contains("volume"))
    {
        if (!float_number(table.get("volume"), value.volume))
        {
            error = "audio_listener.volume must be a finite number";
            return false;
        }
    }
    component.payload.Set(std::move(value));
    return true;
}
void AudioListenerCodec::write(const schema::ComponentDataT& component, toml::table& table) const
{
    ENGINE_PROFILE_FUNCTION();
    const auto* value = component.payload.AsAudioListenerData();
    table.insert("type", "audio_listener");
    table.insert("volume", value->volume);
}
} // namespace engine::editor::asset_pipeline
