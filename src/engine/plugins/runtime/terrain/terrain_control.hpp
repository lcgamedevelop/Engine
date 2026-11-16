#pragma once
#include "plugins/runtime/terrain/terrain_component.hpp"
#include <nlohmann/json_fwd.hpp>
namespace engine::plugins::terrain::control
{
// Protocol adapter shared by script and editor MCP; native operations remain the sole state owner.
[[nodiscard]] bool call(TerrainComponent component, std::string_view method,
                        const nlohmann::json& arguments, nlohmann::json& output,
                        std::string& error);
void abandon_operation(TerrainComponent component, const nlohmann::json& identity);
} // namespace engine::plugins::terrain::control
