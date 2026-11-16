#pragma once
#include <filesystem>
namespace engine::editor
{
class EditorCommands;
}
namespace engine::plugins::terrain
{
class TerrainAuthoringSystem;
[[nodiscard]] bool register_terrain_authoring_commands(TerrainAuthoringSystem& authoring,
                                                       editor::EditorCommands& commands,
                                                       const std::filesystem::path& project);
} // namespace engine::plugins::terrain
