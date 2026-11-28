#pragma once

#include "runtime/resource/resource_system.hpp"

namespace engine::plugins::asset_build
{
// Rewrites the entity and catalog storage metadata only after successful validation.
[[nodiscard]] bool pack_texture(schema::AssetEntryT& entry, ByteBuffer& entity, std::string& error);
} // namespace engine::plugins::asset_build
