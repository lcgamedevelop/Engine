#include "runtime/scene/audio_listener.hpp"
#include "runtime/performance/profiling.hpp"

#include "runtime/scene/ecs_storage.hpp"

#include <cmath>

namespace engine
{

bool AudioListener::register_type(ComponentRegistry& registry)
{
    ENGINE_PROFILE_FUNCTION();
    return register_component_storage<AudioListener, Data>(registry);
}

bool AudioListener::valid_settings(const schema::AudioListenerDataT& settings) noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return std::isfinite(settings.volume) && settings.volume >= 0.0F && settings.volume <= 1.0F;
}

bool AudioListener::get_settings(Data& output) const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    const auto* value = read_data<Data>();
    output = value ? *value : Data{};
    return value != nullptr;
}

bool AudioListener::set_settings(const schema::AudioListenerDataT& settings) noexcept
{
    ENGINE_PROFILE_FUNCTION();
    if (!writable())
    {
        return false;
    }
    if (!valid_settings(settings))
    {
        return false;
    }
    *write_data<Data>() = settings;
    modified();
    return true;
}

bool AudioListener::restore_data(const schema::ComponentDataT& data)
{
    ENGINE_PROFILE_FUNCTION();
    if (!data.flags)
    {
        return false;
    }
    if (!writable())
    {
        return false;
    }
    const auto* settings = data.payload.AsAudioListenerData();
    if (settings == nullptr || !set_settings(*settings))
    {
        return false;
    }
    if (!restore_flags(data))
    {
        return false;
    }
    return true;
}

} // namespace engine
