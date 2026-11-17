#pragma once
#include "plugins/runtime/terrain/terrain_fragment.hpp"
#include "plugins/runtime/terrain/terrain_mesher.hpp"
#include "plugins/runtime/terrain/terrain_operations.hpp"
#include "plugins/runtime/terrain/terrain_state_store.hpp"

namespace engine::plugins::terrain
{
[[nodiscard]] bool rebuild_fragment(const TerrainReadSnapshot& input, const TerrainMesher& mesher,
                                    TerrainFragment& output, std::string& error);
[[nodiscard]] bool stage_collapse(const TerrainReadSnapshot& input, const TerrainCollapseSpec& spec,
                                  const TerrainMesher& mesher, TerrainDelta& output,
                                  std::vector<TerrainFragment>& fragments,
                                  const std::shared_ptr<std::atomic<bool>>& cancelled,
                                  std::string& error);
[[nodiscard]] bool restore_fragments(const TerrainReadSnapshot& source,
                                     const schema::TerrainCheckpointDataT& checkpoint,
                                     const TerrainStateStore& store, std::string_view slot,
                                     const TerrainMesher& mesher,
                                     std::vector<TerrainFragment>& output, std::string& error);
} // namespace engine::plugins::terrain
