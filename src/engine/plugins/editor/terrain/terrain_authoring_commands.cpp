#include "plugins/editor/terrain/terrain_authoring_commands.hpp"
#include "editor/asset_pipeline/asset_io.hpp"
#include "editor/commands/editor_commands.hpp"
#include "editor/commands/editor_edit.hpp"
#include "editor/file_text.hpp"
#include "plugins/editor/terrain/terrain_authoring_system.hpp"
#include "plugins/editor/terrain/terrain_files.hpp"
#include "plugins/editor/terrain/terrain_source.hpp"
#include <charconv>
#include <nlohmann/json.hpp>
namespace engine::plugins::terrain
{
namespace
{
using Json = nlohmann::json;
bool decimal(const Json& value, std::uint64_t& output)
{
    output = 0;
    if (!value.is_string())
    {
        return false;
    }
    const auto& text = value.get_ref<const std::string&>();
    const auto result = std::from_chars(text.data(), text.data() + text.size(), output);
    return !text.empty() && result.ec == std::errc{} && result.ptr == text.data() + text.size();
}
bool identity(const Json& json, TerrainAuthoringId& output)
{
    return json.is_object() && json.size() == 2 && json.contains("session") &&
           json.contains("sequence") && decimal(json["session"], output.session) &&
           decimal(json["sequence"], output.sequence);
}
Json snapshot(const TerrainAuthoringSnapshot& value)
{
    constexpr const char* states[]{"queued",    "preparing", "ready_to_publish",
                                   "published", "failed",    "cancelled"};
    return {{"id",
             {{"session", std::to_string(value.id.session)},
              {"sequence", std::to_string(value.id.sequence)}}},
            {"state", states[unsigned(value.state)]},
            {"completed", std::to_string(value.completed)},
            {"total", std::to_string(value.total)},
            {"sourceRevision", std::to_string(value.source_revision)},
            {"path", value.path},
            {"guid", value.guid},
            {"contentHash", value.content_hash},
            {"canCancel", value.can_cancel},
            {"canPublish", value.can_publish},
            {"errorCode", value.error_code},
            {"error", value.error}};
}
} // namespace
bool register_terrain_authoring_commands(TerrainAuthoringSystem& authoring,
                                         editor::EditorCommands& commands,
                                         const std::filesystem::path& project)
{
    if (!commands.register_command(
            "get_terrain_capabilities", false,
            [](const Json& args, Json& result, std::unique_ptr<editor::EditorEdit>&, std::string&)
            {
                if (!args.empty())
                {
                    return false;
                }
                result = {
                    {"sourceVersion", 2},
                    {"operations",
                     {"generate", "noise_graph", "heightmap", "stamp", "sculpt", "paint", "road"}},
                    {"maximumJobs", 32},
                    {"maximumSourceBytes", 4 * 1024 * 1024},
                    {"maximumRegions", 65536},
                    {"publication", "explicit_undoable_source_transaction"},
                    {"importRequired", true}};
                return true;
            }) ||
        !commands.register_command(
            "get_terrain_source", false,
            [project](const Json& args, Json& result, std::unique_ptr<editor::EditorEdit>&,
                      std::string& error)
            {
                if (!args.is_object() || args.size() != 1 || !args.contains("path") ||
                    !args["path"].is_string())
                {
                    return false;
                }
                std::filesystem::path path;
                schema::TerrainSourceDataT source;
                std::string text;
                if (!terrain_file_path(project, "assets", args["path"].get<std::string>(),
                                       ".terrain", path, error) ||
                    !editor::file_detail::read_text(path, text) ||
                    !read_terrain_source(text, source, error))
                {
                    return false;
                }
                result = {{"path", args["path"]},
                          {"source", text},
                          {"sourceRevision", std::to_string(source.revision)},
                          {"contentHash", editor::asset_pipeline::hash_text(text)}};
                return true;
            }) ||
        !commands.register_command(
            "start_terrain_authoring", false,
            [&authoring](const Json& args, Json& result, std::unique_ptr<editor::EditorEdit>&,
                         std::string& error)
            {
                if (!args.is_object() || args.size() != 3 || !args.contains("path") ||
                    !args["path"].is_string() || !args.contains("source") ||
                    !args["source"].is_string() || !args.contains("expectedSourceRevision"))
                {
                    return false;
                }
                TerrainAuthoringSpec spec;
                TerrainAuthoringId id;
                spec.path = args["path"].get<std::string>();
                if (!decimal(args["expectedSourceRevision"], spec.expected_source_revision) ||
                    !read_terrain_source(args["source"].get_ref<const std::string&>(), spec.source,
                                         error) ||
                    !authoring.start(spec, id))
                {
                    return false;
                }
                result = {{"session", std::to_string(id.session)},
                          {"sequence", std::to_string(id.sequence)}};
                return true;
            }) ||
        !commands.register_command("get_terrain_authoring", false,
                                   [&authoring](const Json& args, Json& result,
                                                std::unique_ptr<editor::EditorEdit>&,
                                                std::string& error)
                                   {
                                       TerrainAuthoringId id;
                                       TerrainAuthoringSnapshot value;
                                       if (!identity(args, id) || !authoring.status(id, value))
                                       {
                                           error = "Unknown or expired terrain authoring identity";
                                           return false;
                                       }
                                       result = snapshot(value);
                                       return true;
                                   }) ||
        !commands.register_command(
            "publish_terrain_authoring", true,
            [&authoring](const Json& args, Json& result, std::unique_ptr<editor::EditorEdit>& edit,
                         std::string& error)
            {
                TerrainAuthoringId id;
                TerrainAuthoringSnapshot value;
                if (!identity(args, id) || !authoring.status(id, value) ||
                    !authoring.make_edit(id, edit))
                {
                    error = "Terrain authoring is not ready or was already issued";
                    return false;
                }
                result = {{"id", args},
                          {"path", value.path},
                          {"guid", value.guid},
                          {"sourceRevision", std::to_string(value.source_revision)},
                          {"importRequired", true}};
                return true;
            }))
    {
        return false;
    }
    for (const auto* name : {"cancel_terrain_authoring", "release_terrain_authoring"})
    {
        const bool cancel = std::string_view(name) == "cancel_terrain_authoring";
        if (!commands.register_command(
                name, false,
                [&authoring, cancel](const Json& args, Json& result,
                                     std::unique_ptr<editor::EditorEdit>&, std::string& error)
                {
                    TerrainAuthoringId id;
                    if (!identity(args, id) ||
                        (cancel ? !authoring.cancel(id) : !authoring.release(id)))
                    {
                        error = "Terrain job is expired or not in an allowed state";
                        return false;
                    }
                    result = {{"success", true}};
                    return true;
                }))
        {
            return false;
        }
    }
    return true;
}
} // namespace engine::plugins::terrain
