#pragma once
#include "plugins/editor/terrain/terrain_generator.hpp"
#include <filesystem>

namespace engine::plugins::terrain
{
// The revision covers the selected generator and the canonical local recipe contract.
inline constexpr auto terrain_generation_revision = "generator-6";
[[nodiscard]] bool generate_terrain_block(const TerrainGenerationInput& input,
                                          const TerrainGenerator& generator,
                                          const std::filesystem::path& cache,
                                          schema::TerrainBlockDataT& output, std::string& error);
} // namespace engine::plugins::terrain
