#include "plugins/editor/google_play/google_play_mcp_tools.hpp"
#include "plugins/editor/mcp/editor_tool_registration.hpp"

namespace engine::plugins::google_play
{
bool register_google_play_tools(mcp::McpSystem& system, mcp::EditorMcpTools& editor)
{
    using Json = nlohmann::json;
    return mcp::register_editor_command_tool(
               system, editor, "start_google_play_release",
               "Inspect current project settings, build its signed APK or AAB outputs, or upload "
               "an AAB to the project's Google Play track.",
               mcp::object_schema(
                   {{"action", {{"type", "string"}, {"enum", {"inspect", "build", "publish"}}}},
                    {"format", {{"type", "string"}, {"enum", {"apk", "aab"}}}}},
                   {"action"}),
               "start_google_play_release") &&
           mcp::register_editor_command_tool(
               system, editor, "get_google_play_status",
               "Read the current Android publishing job and its final receipt.",
               mcp::object_schema(Json::object(), Json::array()), "get_google_play_status");
}
} // namespace engine::plugins::google_play
