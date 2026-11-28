#pragma once

#include "runtime/schema/texture_generated.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace engine::texture_storage
{
inline constexpr std::size_t chunk_bytes = 256U * 1024U;

// Returns the native texel byte count or compressed block byte count.
[[nodiscard]] unsigned unit_bytes(schema::TextureFormat format) noexcept;
// Validates the logical and physical ranges without expanding the texture.
[[nodiscard]] bool validate_chunks(const schema::TextureDataT& texture,
                                   std::uint64_t& logical_bytes);
// Callers hold a validated texture and supply their own upload or CPU destination.
[[nodiscard]] bool read_native(const schema::TextureDataT& texture, std::uint64_t offset,
                               std::span<std::uint8_t> output);
// View borrows the native payload or the caller's scratch until either one changes.
[[nodiscard]] std::span<const std::uint8_t>
native_subresource(const schema::TextureDataT& texture, const schema::TextureSubresourceData& part,
                   std::vector<std::uint8_t>& scratch);
} // namespace engine::texture_storage
