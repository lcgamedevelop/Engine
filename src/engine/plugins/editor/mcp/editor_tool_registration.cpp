#include "plugins/editor/mcp/editor_tool_registration.hpp"
#include "runtime/performance/function_profile.hpp"
#include "runtime/performance/profiling.hpp"

#include "plugins/editor/mcp/editor_mcp_tools.hpp"
#include "plugins/runtime/mcp/mcp_system.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <utility>

namespace engine::plugins::mcp
{
bool register_editor_command_tool(McpSystem& system, EditorMcpTools& editor, std::string name,
                                  std::string description, nlohmann::json input_schema,
                                  std::string command)
{
    ENGINE_PROFILE_FUNCTION();
    if (command.empty())
    {
        return false;
    }
    return system.register_tool({std::move(name), std::move(description), std::move(input_schema),
                                 [&editor, command = std::move(command)](
                                     const nlohmann::json& arguments, nlohmann::json& reply)
                                 {
                                     ENGINE_PROFILE_FUNCTION_PROBE();
                                     return editor.call_command(command, arguments, reply);
                                 }});
}

nlohmann::json object_schema(nlohmann::json properties, nlohmann::json required)
{
    ENGINE_PROFILE_FUNCTION();
    return {{"type", "object"},
            {"properties", std::move(properties)},
            {"required", std::move(required)},
            {"additionalProperties", false}};
}
namespace tools
{
nlohmann::json component_schema(nlohmann::json properties, nlohmann::json required)
{
    ENGINE_PROFILE_FUNCTION();
    properties["sceneId"] = {{"type", "string"}, {"minLength", 1}};
    properties["nodeId"] = {{"type", "string"}, {"minLength", 1}};
    required.push_back("sceneId");
    required.push_back("nodeId");
    return object_schema(std::move(properties), std::move(required));
}
bool component_tool(McpSystem& system, EditorMcpTools& editor, std::string name,
                    std::string description, nlohmann::json properties, nlohmann::json required)
{
    ENGINE_PROFILE_FUNCTION();
    const auto command = name;
    const bool registered = register_editor_command_tool(
        system, editor, std::move(name), std::move(description),
        component_schema(std::move(properties), std::move(required)), command);
    if (!registered)
    {
        std::fprintf(stderr, "Cannot register component MCP tool: %s\n", command.c_str());
    }
    return registered;
}
} // namespace tools
} // namespace engine::plugins::mcp
