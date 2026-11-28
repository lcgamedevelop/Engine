#include "editor/asset_pipeline/texture_compaction.hpp"

#include "runtime/schema/texture_generated.h"

#include <bimg/bimg.h>
#include <bx/allocator.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <optional>

namespace engine::editor::asset_pipeline
{
void compact_opaque_etc2(schema::TextureDataT& texture)
{
    if (texture.encoding != schema::TextureEncoding::native ||
        texture.format != schema::TextureFormat::etc2 || !texture.chunks.empty())
    {
        return;
    }
    bx::DefaultAllocator allocator;
    std::array<std::uint8_t, 4 * 4 * 4> pixels{};
    std::optional<std::uint64_t> previous_opaque;
    for (const auto& part : texture.subresources)
    {
        const auto width = std::max(1U, texture.width >> part.mip());
        const auto height = std::max(1U, texture.height >> part.mip());
        const auto* block = texture.data.data() + part.offset();
        for (std::uint32_t y = 0; y < height; y += 4)
        {
            for (std::uint32_t x = 0; x < width; x += 4, block += 16)
            {
                // An EAC multiplier of zero gives the base alpha for every texel.
                if ((block[1] & 0xf0) == 0)
                {
                    if (block[0] != 255)
                    {
                        return;
                    }
                    continue;
                }
                std::uint64_t alpha;
                std::memcpy(&alpha, block, sizeof(alpha));
                if (previous_opaque == alpha)
                {
                    continue;
                }
                bimg::imageDecodeToRgba8(&allocator, pixels.data(), block, 4, 4, 16,
                                         bimg::TextureFormat::ETC2A);
                for (std::uint32_t row = 0; row < std::min(4U, height - y); ++row)
                {
                    for (std::uint32_t col = 0; col < std::min(4U, width - x); ++col)
                    {
                        if (pixels[(row * 4 + col) * 4 + 3] != 255)
                        {
                            return;
                        }
                    }
                }
                // Only a complete 4x4 block proves the alpha bits opaque at every position.
                if (x + 4 <= width && y + 4 <= height)
                {
                    previous_opaque = alpha;
                }
            }
        }
    }
    // ETC2 RGBA stores an independent EAC alpha block followed by the original RGB block.
    for (std::size_t src = 0; src < texture.data.size(); src += 16)
    {
        std::memcpy(texture.data.data() + src / 2, texture.data.data() + src + 8, 8);
    }
    texture.data.resize(texture.data.size() / 2);
    for (auto& part : texture.subresources)
    {
        part = schema::TextureSubresourceData(part.mip(), part.face(), part.offset() / 2,
                                              part.size() / 2);
    }
    texture.format = schema::TextureFormat::etc2_rgb;
}
} // namespace engine::editor::asset_pipeline
