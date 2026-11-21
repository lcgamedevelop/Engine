#include "editor/asset_pipeline/components/light_probe_volume_codec.hpp"
#include "runtime/performance/profiling.hpp"
#include "runtime/core/guid_map.hpp"

namespace engine::editor::asset_pipeline
{
schema::ComponentPayload LightProbeVolumeCodec::type() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return schema::ComponentPayload::LightProbeVolumeData;
}
std::string_view LightProbeVolumeCodec::name() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return "light_probe_volume";
}
bool LightProbeVolumeCodec::validate(const schema::ComponentDataT& component,
                                     std::string& error) const
{
    ENGINE_PROFILE_FUNCTION();
    const auto* settings = component.payload.AsLightProbeVolumeData();
    if (!settings ||
        (!settings->probes_guid.empty() && !GuidMap::valid_guid(settings->probes_guid)))
    {
        error = "light_probe_volume probes must be a resource GUID";
        return false;
    }
    return true;
}
bool LightProbeVolumeCodec::read(const toml::table& table, schema::ComponentDataT& component,
                                 std::string& error) const
{
    ENGINE_PROFILE_FUNCTION();
    schema::LightProbeVolumeDataT settings;
    if (table.contains("probes"))
    {
        const auto guid = table["probes"].value_exact<std::string>();
        if (!guid)
        {
            error = "light_probe_volume probes must be a resource GUID string";
            return false;
        }
        settings.probes_guid = *guid;
    }
    component.payload.Set(settings);
    return validate(component, error);
}
std::span<const std::string_view> LightProbeVolumeCodec::resource_fields() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    static constexpr std::string_view fields[]{"probes"};
    return fields;
}

void LightProbeVolumeCodec::write(const schema::ComponentDataT& component, toml::table& table) const
{
    ENGINE_PROFILE_FUNCTION();
    table.insert("type", "light_probe_volume");
    table.insert("probes", component.payload.AsLightProbeVolumeData()->probes_guid);
}
} // namespace engine::editor::asset_pipeline
