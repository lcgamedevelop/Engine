#include "plugins/editor/terrain/terrain_surface_import.hpp"
#include "runtime/performance/function_profile.hpp"
#include "runtime/performance/profiling.hpp"
#include "editor/asset_pipeline/asset_io.hpp"
#include "editor/asset_pipeline/importer_utils.hpp"
#include "runtime/resource/builtin_resources.hpp"
#include "runtime/resource/codecs/texture_resource_codec.hpp"
#include "runtime/scene/scene_types.hpp"
#include "runtime/schema/material_generated.h"
#include "runtime/schema/texture_generated.h"
#include <algorithm>
#include <array>
#include <basisu/transcoder/basisu_transcoder.h>
#include <bx/math.h>
#include <cmath>
#include <flatbuffers/flatbuffers.h>
#include <map>

namespace engine::plugins::terrain
{
namespace
{
namespace io = editor::asset_pipeline;
constexpr std::uint32_t resolution = 128;
using Pixel = std::array<float, 4>;

bool load(const Guid& guid, const io::ImportContext& context, ByteBuffer& bytes, std::string& error)
{
    ENGINE_PROFILE_FUNCTION();
    if (builtin_resource_info(guid))
    {
        return builtin_resource_payload(guid, bytes);
    }
    return io::read_generated_payload(guid, context, bytes, error);
}
bool layers(const schema::TerrainGenerationSpecT& recipe, const io::ImportContext& context,
            std::vector<schema::MaterialDataT>& output, std::string& error)
{
    ENGINE_PROFILE_FUNCTION();
    for (const auto& guid : recipe.material_layers)
    {
        ByteBuffer bytes;
        if (!load(guid, context, bytes, error))
        {
            return false;
        }
        flatbuffers::Verifier verifier(reinterpret_cast<const std::uint8_t*>(bytes.data()),
                                       bytes.size());
        if (!schema::VerifyMaterialDataBuffer(verifier))
        {
            error = "Invalid terrain layer material";
            return false;
        }
        auto material = schema::UnPackMaterialData(bytes.data());
        if (material->shader_path != "builtin://shaders/pbr" ||
            material->blend_mode != schema::BlendMode::opaque)
        {
            error = "Terrain layers require opaque builtin PBR materials";
            return false;
        }
        for (const auto& p : material->parameters)
        {
            if ((p->name == "specularWorkflow" || p->name == "alphaCutoff" ||
                 p->name == "hexTilingEnabled") &&
                p->value.AsFloatValue() && p->value.AsFloatValue()->value != 0)
            {
                error = "Terrain layers require opaque metallic-roughness PBR without hex tiling";
                return false;
            }
        }
        for (const auto& t : material->textures)
        {
            if (t->name == "emissionTexture" && t->texture_path != "builtin://textures/white-srgb")
            {
                error = "Terrain layers support emission color but not emission textures";
                return false;
            }
        }
        output.push_back(std::move(*material));
    }
    return !output.empty();
}
float scalar(const schema::MaterialDataT& material, std::string_view name, float fallback)
{
    ENGINE_PROFILE_FUNCTION();
    for (const auto& p : material.parameters)
    {
        if (p->name == name && p->value.AsFloatValue())
        {
            return p->value.AsFloatValue()->value;
        }
    }
    return fallback;
}
Vec4 vector(const schema::MaterialDataT& material, std::string_view name, Vec4 fallback)
{
    ENGINE_PROFILE_FUNCTION();
    for (const auto& p : material.parameters)
    {
        if (p->name == name && p->value.AsVec4Value())
        {
            return p->value.AsVec4Value()->value;
        }
    }
    return fallback;
}
Vec4 emission(const schema::MaterialDataT& material)
{
    ENGINE_PROFILE_FUNCTION();
    for (const auto& p : material.parameters)
    {
        if (p->name == "emissionColor" && p->value.AsVec3Value())
        {
            const auto& v = p->value.AsVec3Value()->value;
            return {v.x(), v.y(), v.z(), 0};
        }
    }
    return {};
}
float linear(float v)
{
    ENGINE_PROFILE_FUNCTION();
    return v <= .04045F ? v / 12.92F : std::pow((v + .055F) / 1.055F, 2.4F);
}
float srgb(float v)
{
    ENGINE_PROFILE_FUNCTION();
    return v <= .0031308F ? v * 12.92F : 1.055F * std::pow(v, 1 / 2.4F) - .055F;
}

bool decode(const schema::MaterialDataT& material, std::string_view name, const char* fallback,
            const io::ImportContext& context, bool color, std::vector<Pixel>& output,
            std::string& error)
{
    ENGINE_PROFILE_FUNCTION();
    Guid guid;
    if (!context.identities.path_to_guid(fallback, guid))
    {
        return false;
    }
    for (const auto& t : material.textures)
    {
        if (t->name == name)
        {
            guid = t->texture_guid;
        }
    }
    ByteBuffer bytes;
    if (!load(guid, context, bytes, error))
    {
        return false;
    }
    flatbuffers::Verifier verifier(reinterpret_cast<const std::uint8_t*>(bytes.data()),
                                   bytes.size());
    if (!schema::VerifyTextureDataBuffer(verifier))
    {
        return false;
    }
    auto image = schema::UnPackTextureData(bytes.data());
    if (!TextureResourceCodec::validate(*image) || image->cube_map || image->array_layers != 1 ||
        image->srgb != color)
    {
        error = "Terrain layer texture has an invalid shape or color space";
        return false;
    }
    std::vector<std::uint8_t> rgba;
    std::uint32_t mip = 0;
    while (mip + 1 < image->mip_count && (image->width >> (mip + 1)) >= resolution &&
           (image->height >> (mip + 1)) >= resolution)
    {
        ++mip;
    }
    auto width = std::max(1U, image->width >> mip), height = std::max(1U, image->height >> mip);
    if (std::uint64_t(width) * height > 16U * 1024U * 1024U)
    {
        error = "Terrain source texture exceeds the decode budget";
        return false;
    }
    rgba.resize(std::size_t(width) * height * 4);
    if (image->encoding == schema::TextureEncoding::basis)
    {
        basist::basisu_transcoder decoder;
        if (!decoder.start_transcoding(image->data.data(), image->data.size()) ||
            !decoder.transcode_image_level(image->data.data(), image->data.size(), 0, mip,
                                           rgba.data(), width * height,
                                           basist::transcoder_texture_format::cTFRGBA32))
        {
            error = "Cannot decode terrain layer texture";
            return false;
        }
    }
    else
    {
        if (image->format != schema::TextureFormat::rgba8)
        {
            error = "Terrain layer sources require Basis or native RGBA8; select these in texture "
                    "import settings";
            return false;
        }
        const auto found =
            std::find_if(image->subresources.begin(), image->subresources.end(),
                         [mip](const auto& p)
                         {
                             ENGINE_PROFILE_FUNCTION_PROBE();
                             return p.mip() == mip && p.face() == 0;
                         });
        if (found == image->subresources.end() || found->size() != rgba.size())
        {
            return false;
        }
        std::copy_n(image->data.data() + found->offset(), rgba.size(), rgba.data());
    }
    output.resize(resolution * resolution);
    auto sample = [&](int x, int y, unsigned c)
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        x = (x % int(width) + int(width)) % int(width);
        y = (y % int(height) + int(height)) % int(height);
        float v = rgba[(std::size_t(y) * width + x) * 4 + c] / 255.F;
        return color && c < 3 ? linear(v) : v;
    };
    for (unsigned y = 0; y < resolution; ++y)
    {
        for (unsigned x = 0; x < resolution; ++x)
        {
            const double u = (x + .5) * width / resolution - .5,
                         v = (y + .5) * height / resolution - .5;
            const int ix = int(std::floor(u)), iy = int(std::floor(v));
            for (unsigned c = 0; c < 4; ++c)
            {
                output[y * resolution + x][c] = std::lerp(
                    std::lerp(sample(ix, iy, c), sample(ix + 1, iy, c), float(u - ix)),
                    std::lerp(sample(ix, iy + 1, c), sample(ix + 1, iy + 1, c), float(u - ix)),
                    float(v - iy));
            }
        }
    }
    return true;
}
void append_mips(schema::TextureDataT& texture, std::vector<Pixel> pixels, unsigned layer,
                 bool normal)
{
    ENGINE_PROFILE_FUNCTION();
    for (unsigned mip = 0, size = resolution; size; size >>= 1, ++mip)
    {
        const auto start = texture.data.size();
        for (auto p : pixels)
        {
            if (normal)
            {
                double x = p[0] * 2 - 1, y = p[1] * 2 - 1, z = p[2] * 2 - 1;
                const auto length = std::hypot(x, y, z);
                if (length > 1e-6)
                {
                    p[0] = float(x / length * .5 + .5);
                    p[1] = float(y / length * .5 + .5);
                    p[2] = float(z / length * .5 + .5);
                }
            }
            for (unsigned c = 0; c < 4; ++c)
            {
                const float v = texture.srgb && c < 3 ? srgb(p[c]) : p[c];
                texture.data.push_back(std::uint8_t(std::lround(std::clamp(v, 0.F, 1.F) * 255)));
            }
        }
        texture.subresources.emplace_back(mip, layer, start, texture.data.size() - start);
        if (size > 1)
        {
            std::vector<Pixel> next(size * size / 4);
            for (unsigned y = 0; y < size / 2; ++y)
            {
                for (unsigned x = 0; x < size / 2; ++x)
                {
                    for (unsigned c = 0; c < 4; ++c)
                    {
                        next[y * (size / 2) + x][c] = (pixels[(y * 2) * size + x * 2][c] +
                                                       pixels[(y * 2) * size + x * 2 + 1][c] +
                                                       pixels[(y * 2 + 1) * size + x * 2][c] +
                                                       pixels[(y * 2 + 1) * size + x * 2 + 1][c]) *
                                                      .25F;
                    }
                }
            }
            pixels = std::move(next);
        }
    }
}
} // namespace

bool build_terrain_surface(const io::ImportAsset& asset, const io::ImportContext& context,
                           const schema::TerrainGenerationSpecT& recipe, std::string& error)
{
    ENGINE_PROFILE_FUNCTION();
    flatbuffers::FlatBufferBuilder builder;
    if (asset.subresource_key == "surface")
    {
        schema::MaterialDataT surface;
        surface.type_path = asset.path;
        surface.shader_path = "builtin://shaders/terrain-pbr";
        if (!context.identities.path_to_guid(surface.shader_path, surface.shader_guid))
        {
            return false;
        }
        const auto root = asset.path.substr(0, asset.path.size() - asset.subresource_key.size());
        for (auto [name, key] :
             {std::pair{"layerBaseColor", "layer-base"}, std::pair{"layerNormal", "layer-normal"},
              std::pair{"layerOrm", "layer-orm"}, std::pair{"layerPalette", "layer-palette"}})
        {
            auto texture = std::make_unique<schema::MaterialTextureT>();
            texture->name = name;
            texture->texture_path = root + key;
            if (!context.identities.path_to_guid(texture->texture_path, texture->texture_guid))
            {
                return false;
            }
            surface.textures.push_back(std::move(texture));
        }
        schema::FinishMaterialDataBuffer(builder, schema::MaterialData::Pack(builder, &surface));
    }
    else
    {
        std::vector<schema::MaterialDataT> materials;
        if (!layers(recipe, context, materials, error))
        {
            return false;
        }
        schema::TextureDataT texture;
        texture.schema_version = 3;
        texture.path = asset.path;
        texture.encoding = schema::TextureEncoding::native;
        if (asset.subresource_key == "layer-palette")
        {
            texture.width = 4;
            texture.height = 256;
            texture.srgb = false;
            texture.format = schema::TextureFormat::rgba16f;
            for (unsigned i = 0; i < 256; ++i)
            {
                const auto& mat = materials[std::min<std::size_t>(i, materials.size() - 1)];
                const auto color = vector(mat, "baseColor", {.7, .3, .15, 1});
                const auto uv = vector(mat, "uvTransform", {1, 1, 0, 0});
                for (const auto& v :
                     {color,
                      Vec4{scalar(mat, "metallic", 0), scalar(mat, "roughness", .4F),
                           scalar(mat, "normalScale", 1), 0},
                      uv, emission(mat)})
                {
                    for (double c : {v.x(), v.y(), v.z(), v.w()})
                    {
                        if (!std::isfinite(c) || std::abs(c) > 65504)
                        {
                            error = "Terrain palette value exceeds half-float range";
                            return false;
                        }
                        auto half = bx::halfFromFloat(float(c));
                        texture.data.push_back(std::uint8_t(half));
                        texture.data.push_back(std::uint8_t(half >> 8));
                    }
                }
            }
            texture.subresources.emplace_back(0, 0, 0, texture.data.size());
        }
        else
        {
            const bool base = asset.subresource_key == "layer-base",
                       normal = asset.subresource_key == "layer-normal";
            texture.width = texture.height = resolution;
            texture.mip_count = 8;
            texture.srgb = base;
            texture.array_layers = std::max<std::uint32_t>(2, materials.size());
            texture.format = schema::TextureFormat::rgba8;
            for (unsigned i = 0; i < texture.array_layers; ++i)
            {
                std::vector<Pixel> pixels;
                if (!decode(materials[std::min<std::size_t>(i, materials.size() - 1)],
                            base     ? "baseColorTexture"
                            : normal ? "normalTexture"
                                     : "ormTexture",
                            base     ? "builtin://textures/white-srgb"
                            : normal ? "builtin://textures/flat-normal"
                                     : "builtin://textures/white-linear",
                            context, base, pixels, error))
                {
                    return false;
                }
                append_mips(texture, std::move(pixels), i, normal);
            }
        }
        if (!TextureResourceCodec::validate(texture))
        {
            error = "Generated terrain surface texture is invalid";
            return false;
        }
        schema::FinishTextureDataBuffer(builder, schema::TextureData::Pack(builder, &texture));
    }
    return io::write_bytes(context.output / asset.guid,
                           std::as_bytes(std::span(builder.GetBufferPointer(), builder.GetSize())),
                           error);
}
} // namespace engine::plugins::terrain
