#pragma once
#include "editor/schema/terrain_authoring_generated.h"
#include "plugins/runtime/terrain/terrain_volume.hpp"
#include <memory>

namespace engine::plugins::terrain
{
struct TerrainGenerationInput
{
    schema::TerrainGenerationSpecT recipe;
    TerrainRegionKey key;
    std::uint64_t source_revision = 0;
};
class TerrainGenerator
{
public:
    virtual ~TerrainGenerator() noexcept = default;
    [[nodiscard]] virtual bool generate(const TerrainGenerationInput& input,
                                        schema::TerrainBlockDataT& output,
                                        std::string& error) const = 0;
};
[[nodiscard]] std::unique_ptr<TerrainGenerator> create_terrain_generator(std::string& error);
} // namespace engine::plugins::terrain
