#include "plugins/editor/terrain/terrain_commands.hpp"
#include "editor/commands/component_commands.hpp"
#include "editor/commands/scene_commands.hpp"
#include "editor/resource_access.hpp"
#include "plugins/editor/terrain/terrain_files.hpp"
#include "plugins/runtime/terrain/terrain_control.hpp"
#include "runtime/schema/control_json_generated.hpp"
#include "runtime/world/world_system.hpp"

namespace engine::plugins::terrain
{
bool register_terrain_commands(WorldSystem& world, editor::SceneCommands& scenes,
                               editor::EditorCommands& commands,
                               const std::filesystem::path& project)
{
    using Json = nlohmann::json;
    if (!register_terrain_asset_commands(commands, project) ||
        !editor::components::register_component<TerrainComponent, schema::TerrainDataT>(
            world, scenes, commands, "terrain",
            [&world](Node, const Json& args, schema::TerrainDataT& data,
                     std::vector<ResourceReference>& prepared, std::string& error)
            {
                if (!editor::components::fields(args, {"settings"}, error) ||
                    !args.contains("settings") ||
                    !schema::control_json::read(args["settings"], data, error))
                {
                    return false;
                }
                for (auto* guid : {&data.terrain_guid, &data.surface_material_guid})
                {
                    if (guid->empty())
                    {
                        continue;
                    }
                    ResourceReference reference;
                    if (!editor::resource_access::load(world.resource_loader(), *guid, reference))
                    {
                        error = "Terrain resource is unavailable";
                        return false;
                    }
                    *guid = reference->info().guid;
                    prepared.push_back(std::move(reference));
                }
                return true;
            },
            [](TerrainComponent, const schema::TerrainDataT& data)
            { return Json{{"settings", schema::control_json::write(data)}}; }))
    {
        return false;
    }
    for (const std::string method :
         {"set_view", "remove_view", "pin_regions", "release_pin", "request_edit",
          "request_collapse", "request_reset", "request_retire", "request_save", "request_load",
          "operation_status", "cancel_operation", "release_operation", "query_regions",
          "collect_support"})
    {
        if (!commands.register_command(
                "terrain_" + method, false,
                [&world, method](const Json& args, Json& result,
                                 std::unique_ptr<editor::EditorEdit>&, std::string& error)
                {
                    if (!editor::components::fields(args, {"request"}, error) ||
                        !args.contains("request"))
                    {
                        return false;
                    }
                    const auto node = editor::components::target(world, args, error);
                    return node && control::call(node.get_component<TerrainComponent>(), method,
                                                 args["request"], result, error);
                }))
        {
            return false;
        }
    }
    return true;
}
} // namespace engine::plugins::terrain
