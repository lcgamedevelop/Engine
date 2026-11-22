#pragma once
namespace engine::plugins::mcp
{
class McpSystem;
class EditorMcpTools;
} // namespace engine::plugins::mcp
namespace engine::plugins::google_play
{
[[nodiscard]] bool register_google_play_tools(mcp::McpSystem& system, mcp::EditorMcpTools& editor);
}
