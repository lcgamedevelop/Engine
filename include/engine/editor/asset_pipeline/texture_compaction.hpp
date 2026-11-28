#pragma once

namespace engine::schema
{
struct TextureDataT;
}

namespace engine::editor::asset_pipeline
{
// Requires validated native layout. Compacts only completely opaque ETC2 RGBA images.
void compact_opaque_etc2(schema::TextureDataT& texture);
}
