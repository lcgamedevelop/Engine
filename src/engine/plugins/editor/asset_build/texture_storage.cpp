#include "plugins/editor/asset_build/texture_storage.hpp"

#include "editor/asset_pipeline/asset_io.hpp"
#include "editor/asset_pipeline/texture_compaction.hpp"
#include "runtime/resource/codecs/texture_chunks.hpp"
#include "runtime/resource/codecs/texture_resource_codec.hpp"

#include <flatbuffers/flatbuffers.h>
#include <zstd.h>

#include <algorithm>
#include <span>
#include <vector>

namespace engine::plugins::asset_build
{
namespace
{
bool compress_native(schema::TextureDataT& texture)
{
    const unsigned unit = texture_storage::unit_bytes(texture.format);
    if (!unit || !texture.chunks.empty())
    {
        return false;
    }
    std::vector<std::uint8_t> packed;
    std::vector<schema::TextureChunk> chunks;
    std::vector<std::uint8_t> planes;
    std::vector<std::uint8_t> compressed(ZSTD_compressBound(texture_storage::chunk_bytes));
    std::vector<std::uint8_t> compressed_planes(compressed.size());
    auto parts = texture.subresources;
    std::sort(parts.begin(), parts.end(),
              [](const auto& left, const auto& right) { return left.offset() < right.offset(); });
    for (const auto& part : parts)
    {
        const std::uint64_t end = part.offset() + part.size();
        for (auto offset = part.offset(); offset < end;)
        {
            const std::size_t size =
                std::min<std::uint64_t>(texture_storage::chunk_bytes, end - offset);
            const auto* data = texture.data.data() + offset;
            std::span<const std::uint8_t> best(data, size);
            auto encoding = schema::TextureChunkEncoding::raw;
            // Small mip tails cost more to describe and initialize than they save.
            if (size >= 64)
            {
                const auto direct =
                    ZSTD_compress(compressed.data(), compressed.size(), data, size, 9);
                if (ZSTD_isError(direct))
                {
                    return false;
                }
                if (direct < best.size())
                {
                    best = {compressed.data(), direct};
                    encoding = schema::TextureChunkEncoding::zstd;
                }
                planes.resize(size);
                const auto stride = size / unit;
                for (unsigned lane = 0; lane < unit; ++lane)
                {
                    for (std::size_t i = 0; i < stride; ++i)
                    {
                        planes[lane * stride + i] = data[i * unit + lane];
                    }
                }
                const auto shuffled = ZSTD_compress(
                    compressed_planes.data(), compressed_planes.size(), planes.data(), size, 9);
                if (ZSTD_isError(shuffled))
                {
                    return false;
                }
                if (shuffled < best.size())
                {
                    best = {compressed_planes.data(), shuffled};
                    encoding = schema::TextureChunkEncoding::shuffled_zstd;
                }
            }
            chunks.emplace_back(static_cast<std::uint32_t>(offset),
                                static_cast<std::uint32_t>(size),
                                static_cast<std::uint32_t>(packed.size()),
                                static_cast<std::uint32_t>(best.size()), encoding);
            packed.insert(packed.end(), best.begin(), best.end());
            offset += size;
        }
    }
    if (packed.size() + chunks.size() * sizeof(schema::TextureChunk) < texture.data.size())
    {
        texture.data = std::move(packed);
        texture.chunks = std::move(chunks);
    }
    return true;
}
} // namespace

bool pack_texture(schema::AssetEntryT& entry, ByteBuffer& entity, std::string& error)
{
    ByteBuffer payload;
    if (!ResourceSystem::decode_resource(entry, entity, payload))
    {
        error = "Cannot read texture entity: " + entry.path;
        return false;
    }
    flatbuffers::Verifier verifier(reinterpret_cast<const std::uint8_t*>(payload.data()),
                                   payload.size());
    if (!schema::VerifyTextureDataBuffer(verifier))
    {
        error = "Invalid texture entity: " + entry.path;
        return false;
    }
    auto texture = schema::UnPackTextureData(payload.data());
    if (!TextureResourceCodec::validate(*texture))
    {
        error = "Invalid texture layout: " + entry.path;
        return false;
    }
    if (texture->encoding != schema::TextureEncoding::native || !texture->chunks.empty())
    {
        return true;
    }
    const auto original_format = texture->format;
    engine::editor::asset_pipeline::compact_opaque_etc2(*texture);
    if (!compress_native(*texture) || !TextureResourceCodec::validate(*texture))
    {
        error = "Cannot compress the native texture: " + entry.path;
        return false;
    }
    if (original_format == texture->format && texture->chunks.empty())
    {
        return true;
    }
    flatbuffers::FlatBufferBuilder builder;
    schema::FinishTextureDataBuffer(builder, schema::TextureData::Pack(builder, texture.get()));
    const ByteView encoded{reinterpret_cast<const std::byte*>(builder.GetBufferPointer()),
                           builder.GetSize()};
    ByteBuffer result;
    std::uint32_t header_size = 0;
    if (!ResourceSystem::encode_resource(entry.guid, entry.kind, encoded, result, header_size))
    {
        error = "Cannot write compressed texture: " + entry.path;
        return false;
    }
    entry.header_size = header_size;
    entry.content_size = result.size();
    entry.location->length = result.size();
    entry.content_hash = engine::editor::asset_pipeline::hash_bytes(result);
    entity = std::move(result);
    return true;
}
} // namespace engine::plugins::asset_build
