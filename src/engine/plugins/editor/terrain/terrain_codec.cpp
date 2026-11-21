#include "plugins/editor/terrain/terrain_codec.hpp"
#include "runtime/performance/profiling.hpp"
#include "editor/asset_pipeline/control_toml.hpp"
#include "editor/asset_pipeline/importer_utils.hpp"
#include "plugins/runtime/terrain/terrain_component.hpp"
#include "runtime/resource/builtin_resources.hpp"

namespace engine::plugins::terrain
{
schema::ComponentPayload TerrainCodec::type() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return schema::ComponentPayload::TerrainData;
}
std::string_view TerrainCodec::name() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return "terrain";
}
bool TerrainCodec::validate(const schema::ComponentDataT& component, std::string& error) const
{
    ENGINE_PROFILE_FUNCTION();
    const auto* data = component.payload.AsTerrainData();
    if (!data || !TerrainComponent::valid_settings(*data))
    {
        error = "Invalid terrain component settings";
        return false;
    }
    return true;
}
bool TerrainCodec::read(const toml::table& table, schema::ComponentDataT& component,
                        std::string& error) const
{
    ENGINE_PROFILE_FUNCTION();
    nlohmann::json json;
    schema::TerrainDataT data;
    if (!editor::asset_pipeline::control_toml::read_component(table, json, error) ||
        !schema::control_json::read(json, data, error))
    {
        return false;
    }
    if (!data.terrain_guid.empty() && data.surface_material_guid.empty())
    {
        for (const auto& entry : builtin_resource_catalog().entries)
        {
            if (entry->path == "builtin://materials/default-pbr")
            {
                data.surface_material_guid = entry->guid;
                break;
            }
        }
    }
    if (!TerrainComponent::valid_settings(data))
    {
        error = "Invalid terrain settings";
        return false;
    }
    component.payload.Set(std::move(data));
    return true;
}
bool TerrainCodec::scan_dependencies(const schema::ComponentDataT& component,
                                     const editor::asset_pipeline::ImportContext& context,
                                     std::vector<Guid>& dependencies, std::string& error) const
{
    ENGINE_PROFILE_FUNCTION();
    if (!validate(component, error))
    {
        return false;
    }
    const auto& data = *component.payload.AsTerrainData();
    return (data.terrain_guid.empty() ||
            editor::asset_pipeline::reference(data.terrain_guid, schema::AssetKind::terrain,
                                              context, dependencies, error)) &&
           (data.surface_material_guid.empty() ||
            editor::asset_pipeline::reference(data.surface_material_guid,
                                              schema::AssetKind::material, context, dependencies,
                                              error));
}
std::span<const std::string_view> TerrainCodec::resource_fields() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    static constexpr std::string_view fields[]{"terrain_guid", "surface_material_guid"};
    return fields;
}
void TerrainCodec::write(const schema::ComponentDataT& component, toml::table& table) const
{
    ENGINE_PROFILE_FUNCTION();
    table.insert_or_assign("type", "terrain");
    editor::asset_pipeline::control_toml::write_table(
        schema::control_json::write(*component.payload.AsTerrainData()), table);
}
bool register_terrain_codec()
{
    ENGINE_PROFILE_FUNCTION();
    if (editor::asset_pipeline::find_scene_component_codec(schema::ComponentPayload::TerrainData))
    {
        return true;
    }
    std::unique_ptr<editor::asset_pipeline::SceneComponentCodec> codec =
        std::make_unique<TerrainCodec>();
    return editor::asset_pipeline::register_scene_component_codec(codec);
}
} // namespace engine::plugins::terrain
