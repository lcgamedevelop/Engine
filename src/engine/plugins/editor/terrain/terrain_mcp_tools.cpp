#include "plugins/editor/terrain/terrain_mcp_tools.hpp"
#include "plugins/editor/mcp/editor_mcp_tools.hpp"
#include "plugins/editor/mcp/editor_tool_registration.hpp"
#include "plugins/runtime/mcp/mcp_system.hpp"
#include "runtime/schema/control_json_generated.hpp"

namespace engine::plugins::terrain
{
bool register_terrain_mcp_tools(mcp::McpSystem& system, mcp::EditorMcpTools& editor)
{
    using Json = nlohmann::json;
    if (!system.register_tool(
            {"create_terrain_asset",
             "Create a schema_version=2 terrain TOML source and unique meta. Use bounds_min/max, "
             "voxel_size, block_cells, lod_count, height, amplitude, frequency, seed, "
             "material_layers, [[stamps]], [[edits]] and [[roads]]. Import with "
             "refresh_scene_preview or sync_assets. Existing sources are never overwritten.",
             {{"type", "object"},
              {"properties",
               {{"path", {{"type", "string"}}},
                {"source", {{"type", "string"}, {"maxLength", 4194304}}}}},
              {"required", Json::array({"path", "source"})},
              {"additionalProperties", false}},
             [&editor](const Json& args, Json& result)
             { return editor.call_command("create_terrain_asset", args, result); }}) ||
        !mcp::tools::component_tool(
            system, editor, "set_terrain",
            "Set terrain component settings. Streaming and collision must be explicitly enabled. "
            "Runtime requests remain asynchronous.",
            {{"settings",
              schema::control_json::describe(static_cast<const schema::TerrainDataT*>(nullptr))}},
            Json::array({"settings"})) ||
        !mcp::tools::component_tool(system, editor, "get_terrain",
                                    "Read the current terrain settings.", Json::object()))
    {
        return false;
    }
    const std::pair<const char*, const char*> methods[]{
        {"set_view", "Set a temporary world-space view: view_key (decimal uint64), view "
                     "{center:{x,y,z},visual_radius,collision_radius,lod_bias,priority}."},
        {"remove_view", "Release a temporary view by view_key."},
        {"pin_regions", "Pin regions: owner_key, reason, collision_required, "
                        "regions:[{x,y,z,lod}]. Returns an identity."},
        {"release_pin", "Release an exact pin identity."},
        {"request_edit",
         "Queue one atomic brush batch: expected_revision optional, "
         "brushes:[{center:{x,y,z},extent:{x,y,z},shape:sphere|box,operation:dig|fill|flatten|"
         "smooth|paint,strength,layer}]. World coordinates. Returns an operation identity."},
        {"request_collapse",
         "Queue bounded connected-island extraction: bounds:{minimum:{x,y,z},maximum:{x,y,z}}, "
         "expected_revision, min_samples, fragment_cells, max_fragments, density, "
         "connectivity_block_limit."},
        {"request_reset", "Queue restoration of complete blocks in bounds. Keeps the existing "
                          "world until publication."},
        {"request_retire",
         "Queue fragment removal: max_count, max_age_seconds and optional expected_revision."},
        {"request_save", "Queue a complete checkpoint: slot, checkpoint_id, optional "
                         "expected_revision. Uses the App persistent root."},
        {"request_load", "Queue checkpoint restoration: slot, checkpoint_id, expected_base_hash. "
                         "Requires matching source identity."},
        {"operation_status", "Read operation state and revision without advancing work. request is "
                             "{world_epoch,node_id,component_instance,sequence}; all identity "
                             "fields are decimal uint64 strings."},
        {"cancel_operation", "Cancel an exact operation identity before publication."},
        {"release_operation",
         "Release a terminal operation record. Does not unload published terrain."},
        {"query_regions", "Read bounded region status: bounds, lod, limit and optional cursor. "
                          "Revisions and identities use decimal uint64 strings."},
        {"collect_support",
         "Return complete current static collider identities for bounds at lod 0. Unknown or stale "
         "collision fails; an empty successful list means known air."}};
    for (const auto& [name, description] : methods)
    {
        if (!mcp::tools::component_tool(system, editor, std::string("terrain_") + name, description,
                                        {{"request", {{"type", "object"}}}},
                                        Json::array({"request"})))
        {
            return false;
        }
    }
    const auto add = [&](const char* name, const char* description, Json properties, Json required)
    {
        return mcp::register_editor_command_tool(
            system, editor, name, description,
            mcp::object_schema(std::move(properties), std::move(required)), name);
    };
    if (!add("get_terrain_capabilities",
             "Read authoring operations, limits and publication semantics.", Json::object(),
             Json::array()) ||
        !add("get_terrain_source", "Read complete source and revision before editing.",
             {{"path", {{"type", "string"}}}}, {"path"}) ||
        !add("start_terrain_authoring",
             "Prepare complete terrain source asynchronously. Source includes ordered sculpt, "
             "paint and road recipes. New revision must be expectedSourceRevision + 1; use 0 only "
             "to create. Query then explicitly publish.",
             {{"path", {{"type", "string"}}},
              {"expectedSourceRevision", {{"type", "string"}}},
              {"source", {{"type", "string"}, {"maxLength", 4194304}}}},
             {"path", "expectedSourceRevision", "source"}))
    {
        return false;
    }
    for (const auto* name : {"get_terrain_authoring", "cancel_terrain_authoring",
                             "publish_terrain_authoring", "release_terrain_authoring"})
    {
        if (!add(name,
                 "Operate on an exact authoring identity. Publication is undoable; import and "
                 "rendering are separate stages.",
                 {{"session", {{"type", "string"}}}, {"sequence", {{"type", "string"}}}},
                 {"session", "sequence"}))
        {
            return false;
        }
    }
    return true;
}
} // namespace engine::plugins::terrain
