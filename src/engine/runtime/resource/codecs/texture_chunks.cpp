#include "runtime/resource/codecs/texture_chunks.hpp"

#include <zstd.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace engine::texture_storage
{
unsigned unit_bytes(schema::TextureFormat format) noexcept
{
    using Format = schema::TextureFormat;
    switch (format)
    {
    case Format::rgba8:
    case Format::rg16f:
        return 4;
    case Format::rgba16f:
    case Format::etc2_rgb:
        return 8;
    case Format::bc7:
    case Format::astc4x4:
    case Format::etc2:
        return 16;
    default:
        return 0;
    }
}

bool validate_chunks(const schema::TextureDataT& texture, std::uint64_t& logical_bytes)
{
    logical_bytes = 0;
    if (texture.chunks.empty())
    {
        logical_bytes = texture.data.size();
        return true;
    }
    const unsigned unit = unit_bytes(texture.format);
    if (texture.encoding != schema::TextureEncoding::native || unit == 0)
    {
        return false;
    }
    std::size_t stored = 0;
    constexpr std::size_t limit = 64U * 1024U * 1024U;
    for (const auto& chunk : texture.chunks)
    {
        if (chunk.offset() != logical_bytes || chunk.stored_offset() != stored ||
            chunk.size() == 0 || chunk.size() > chunk_bytes || chunk.size() % unit != 0 ||
            chunk.size() > limit - logical_bytes || chunk.stored_size() == 0 ||
            chunk.stored_size() > texture.data.size() - stored)
        {
            return false;
        }
        switch (chunk.encoding())
        {
        case schema::TextureChunkEncoding::raw:
            if (chunk.stored_size() != chunk.size())
            {
                return false;
            }
            break;
        case schema::TextureChunkEncoding::zstd:
        case schema::TextureChunkEncoding::shuffled_zstd:
            if (ZSTD_getFrameContentSize(texture.data.data() + stored, chunk.stored_size()) !=
                chunk.size())
            {
                return false;
            }
            break;
        default:
            return false;
        }
        logical_bytes += chunk.size();
        stored += chunk.stored_size();
    }
    return stored == texture.data.size();
}

namespace
{
template <unsigned Width>
void unshuffle(const std::uint8_t* planes, std::uint8_t* output, std::size_t size,
               std::size_t start, std::size_t count)
{
    const auto stride = size / Width;
    for (std::size_t i = 0; i != count / Width; ++i)
    {
        for (unsigned lane = 0; lane != Width; ++lane)
        {
            output[i * Width + lane] = planes[lane * stride + start / Width + i];
        }
    }
}
} // namespace

bool read_native(const schema::TextureDataT& texture, std::uint64_t offset,
                 std::span<std::uint8_t> output)
{
    if (texture.encoding != schema::TextureEncoding::native)
    {
        return false;
    }
    if (texture.chunks.empty())
    {
        if (offset > texture.data.size() || output.size() > texture.data.size() - offset)
        {
            return false;
        }
        std::copy_n(texture.data.data() + offset, output.size(), output.data());
        return true;
    }
    const auto unit = unit_bytes(texture.format);
    if (!unit || offset % unit || output.size() % unit)
    {
        return false;
    }
    auto it = std::lower_bound(texture.chunks.begin(), texture.chunks.end(), offset,
                               [](const auto& chunk, auto at)
                               { return std::uint64_t(chunk.offset()) + chunk.size() <= at; });
    std::vector<std::uint8_t> scratch;
    while (!output.empty() && it != texture.chunks.end())
    {
        const auto& chunk = *it++;
        if (offset < chunk.offset() || offset >= std::uint64_t(chunk.offset()) + chunk.size() ||
            chunk.size() > chunk_bytes || chunk.stored_offset() > texture.data.size() ||
            chunk.stored_size() > texture.data.size() - chunk.stored_offset())
        {
            return false;
        }
        const auto start = static_cast<std::size_t>(offset - chunk.offset());
        const auto count = std::min<std::size_t>(chunk.size() - start, output.size());
        const auto* source = texture.data.data() + chunk.stored_offset();
        if (chunk.encoding() == schema::TextureChunkEncoding::raw)
        {
            if (chunk.stored_size() != chunk.size())
            {
                return false;
            }
            std::memcpy(output.data(), source + start, count);
        }
        else
        {
            const bool shuffle = chunk.encoding() == schema::TextureChunkEncoding::shuffled_zstd;
            const bool direct = !shuffle && start == 0 && count == chunk.size();
            if (!direct)
            {
                scratch.resize(chunk.size());
            }
            auto* decoded = direct ? output.data() : scratch.data();
            if (ZSTD_decompress(decoded, chunk.size(), source, chunk.stored_size()) != chunk.size())
            {
                return false;
            }
            if (shuffle)
            {
                switch (unit)
                {
                case 4:
                    unshuffle<4>(decoded, output.data(), chunk.size(), start, count);
                    break;
                case 8:
                    unshuffle<8>(decoded, output.data(), chunk.size(), start, count);
                    break;
                case 16:
                    unshuffle<16>(decoded, output.data(), chunk.size(), start, count);
                    break;
                default:
                    return false;
                }
            }
            else if (!direct)
            {
                std::memcpy(output.data(), decoded + start, count);
            }
        }
        offset += count;
        output = output.subspan(count);
    }
    return output.empty();
}
std::span<const std::uint8_t> native_subresource(const schema::TextureDataT& texture,
                                                 const schema::TextureSubresourceData& part,
                                                 std::vector<std::uint8_t>& scratch)
{
    if (texture.encoding != schema::TextureEncoding::native || part.size() == 0 ||
        part.size() > 64U * 1024U * 1024U)
    {
        return {};
    }
    if (texture.chunks.empty())
    {
        if (part.offset() > texture.data.size() ||
            part.size() > texture.data.size() - part.offset())
        {
            return {};
        }
        return {texture.data.data() + part.offset(), static_cast<std::size_t>(part.size())};
    }
    scratch.resize(static_cast<std::size_t>(part.size()));
    if (!read_native(texture, part.offset(), scratch))
    {
        return {};
    }
    return scratch;
}
} // namespace engine::texture_storage
