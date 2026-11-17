#pragma once
#include "editor/asset_pipeline/import_context.hpp"
#include "editor/schema/terrain_authoring_generated.h"
namespace engine::plugins::terrain
{
[[nodiscard]] bool build_terrain_surface(const editor::asset_pipeline::ImportAsset& asset,
                                         const editor::asset_pipeline::ImportContext& context,
                                         const schema::TerrainGenerationSpecT& recipe,
                                         std::string& error);
}
