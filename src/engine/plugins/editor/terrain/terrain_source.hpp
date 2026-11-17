#pragma once
#include "plugins/editor/terrain/terrain_generator.hpp"
#include <filesystem>
#include <string_view>

namespace engine::plugins::terrain
{
[[nodiscard]] bool read_terrain_source(std::string_view text, schema::TerrainSourceDataT& output,
                                       std::string& error);
[[nodiscard]] bool write_terrain_source(const schema::TerrainSourceDataT& source,
                                        std::string& output, std::string& error);
[[nodiscard]] bool read_terrain_file(const std::filesystem::path& path,
                                     schema::TerrainSourceDataT& output, std::string& error);
[[nodiscard]] bool terrain_source_keys(const schema::TerrainSourceDataT& source,
                                       std::vector<TerrainRegionKey>& keys, std::string& error);
[[nodiscard]] std::string terrain_region_name(TerrainRegionKey key);
[[nodiscard]] bool terrain_region_key(std::string_view name, TerrainRegionKey& output);
} // namespace engine::plugins::terrain
