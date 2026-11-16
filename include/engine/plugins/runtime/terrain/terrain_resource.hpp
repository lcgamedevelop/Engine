#pragma once

#include "runtime/resource/resource.hpp"
#include "runtime/schema/terrain_block_generated.h"
#include "runtime/schema/terrain_generated.h"

namespace engine::resource_detail
{
template <> struct Payload<schema::AssetKind::terrain>
{
    using Type = schema::TerrainAssetDataT;
};
template <> struct Payload<schema::AssetKind::terrain_block>
{
    using Type = schema::TerrainBlockDataT;
};
} // namespace engine::resource_detail
namespace engine::plugins::terrain
{
using TerrainResource = ResourceValue<schema::TerrainAssetDataT, schema::AssetKind::terrain>;
using TerrainBlockResource =
    ResourceValue<schema::TerrainBlockDataT, schema::AssetKind::terrain_block>;
} // namespace engine::plugins::terrain
