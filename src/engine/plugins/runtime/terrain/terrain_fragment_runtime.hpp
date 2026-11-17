#pragma once
#include "plugins/runtime/terrain/terrain_component_runtime.hpp"

namespace engine::plugins::terrain
{
[[nodiscard]] bool create_fragments(Scene& scene, TerrainComponent owner,
                                    const TransformSettings& pose,
                                    const schema::TerrainDataT& settings,
                                    std::span<const TerrainFragment> fragments,
                                    std::vector<Node>& output, std::string& error);
[[nodiscard]] bool capture_fragments(std::span<const TerrainComponentRuntime::Fragment> fragments,
                                     double time, schema::TerrainCheckpointDataT& output,
                                     std::vector<schema::TerrainBlockDataT>& blocks,
                                     std::string& error);
} // namespace engine::plugins::terrain
