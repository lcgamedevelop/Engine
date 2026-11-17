#pragma once
#include "plugins/editor/terrain/terrain_generator.hpp"
#include <filesystem>

namespace engine::plugins::terrain
{
// Canonical local recipe/cache format; the selected generator supplies its own identity.
inline constexpr auto terrain_generation_revision = "blocks-7";
[[nodiscard]] bool generate_terrain_block(const TerrainGenerationInput& input,
                                          const TerrainGenerator& generator,
                                          const std::filesystem::path& cache,
                                          schema::TerrainBlockDataT& output, std::string& error);
} // namespace engine::plugins::terrain
