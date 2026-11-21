#include "plugins/editor/terrain/terrain_importer.hpp"
#include "runtime/performance/function_profile.hpp"
#include "runtime/performance/profiling.hpp"
#include "editor/asset_pipeline/asset_io.hpp"
#include "editor/asset_pipeline/importer_utils.hpp"
#include "editor/file_text.hpp"
#include "plugins/editor/terrain/terrain_generation_cache.hpp"
#include "plugins/editor/terrain/terrain_source.hpp"
#include "plugins/editor/terrain/terrain_surface_import.hpp"
#include "runtime/resource/resource_system.hpp"
#include <algorithm>
#include <flatbuffers/flatbuffers.h>

namespace engine::plugins::terrain
{
namespace
{
using namespace editor::asset_pipeline;
bool payload(const ImportContext& context, const Guid& guid, ByteBuffer& output, std::string& error)
{
    ENGINE_PROFILE_FUNCTION();
    if (!read_bytes(context.output / guid, output, error))
    {
        return false;
    }
    // Reused incremental artifacts are already wrapped; newly converted ones are raw payloads.
    if (output.size() >= 8 && (schema::TerrainBlockDataBufferHasIdentifier(output.data()) ||
                               schema::TerrainAssetDataBufferHasIdentifier(output.data())))
    {
        return true;
    }
    if (context.previous_catalog)
    {
        for (const auto& entry : context.previous_catalog->entries)
        {
            if (entry->guid != guid)
            {
                continue;
            }
            ByteBuffer raw;
            if (!ResourceSystem::decode_resource(*entry, output, raw))
            {
                break;
            }
            output = std::move(raw);
            return true;
        }
    }
    error = "Cannot decode the staged terrain dependency";
    return false;
}
bool identities(const ImportAsset& asset, const ImportContext& context,
                const std::vector<TerrainRegionKey>& keys, std::vector<Guid>& output,
                std::string& error)
{
    ENGINE_PROFILE_FUNCTION();
    output.clear();
    for (auto key : keys)
    {
        Guid guid;
        if (!context.identities.path_to_guid(asset.path + "/" + terrain_region_name(key), guid))
        {
            error = "Missing terrain region identity";
            return false;
        }
        output.push_back(std::move(guid));
    }
    return true;
}
} // namespace
TerrainImporter::TerrainImporter(std::unique_ptr<TerrainGenerator> generator) noexcept
    : generator_(std::move(generator))
{
    ENGINE_PROFILE_FUNCTION();
}

std::string_view TerrainImporter::id() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return "terrain";
}
schema::AssetKind TerrainImporter::kind() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return schema::AssetKind::terrain;
}
bool TerrainImporter::matches(const std::filesystem::path& path) const
{
    ENGINE_PROFILE_FUNCTION();
    return editor::asset_pipeline::source_extension(path) == ".terrain";
}
bool TerrainImporter::discover(const ImportAsset& asset, std::vector<ImportSubresource>& output,
                               std::string& error) const
{
    ENGINE_PROFILE_FUNCTION();
    output.clear();
    schema::TerrainSourceDataT source;
    std::vector<TerrainRegionKey> keys;
    if (!read_terrain_file(asset.source, source, error) ||
        !terrain_source_keys(source, keys, error))
    {
        return false;
    }
    for (auto key : keys)
    {
        output.push_back({terrain_region_name(key), schema::AssetKind::terrain_block});
    }
    if (!source.recipe->material_layers.empty())
    {
        for (auto name : {"layer-base", "layer-normal", "layer-orm", "layer-palette"})
        {
            output.push_back({name, schema::AssetKind::texture});
        }
        output.push_back({"surface", schema::AssetKind::material});
    }
    return true;
}
bool TerrainImporter::inspect_batch(const ImportContext&, std::string& fingerprint,
                                    std::string& error)
{
    ENGINE_PROFILE_FUNCTION();
    if (!generator_ || generator_->fingerprint().empty())
    {
        error = "Terrain import requires an injected generator with a fingerprint";
        return false;
    }
    if (!schema_fingerprint(
            {"runtime/terrain.fbs", "runtime/terrain_block.fbs", "editor/terrain_authoring.fbs"},
            fingerprint, error))
    {
        return false;
    }
    fingerprint += ":sparse-terrain-7:" + std::string(generator_->fingerprint());
    return true;
}
bool TerrainImporter::scan_dependencies(ImportAsset& asset, const ImportContext& context,
                                        std::string& error) const
{
    ENGINE_PROFILE_FUNCTION();
    asset.build_dependencies.clear();
    asset.runtime_dependencies.clear();
    asset.stream_dependencies.clear();
    schema::TerrainSourceDataT source;
    if (!read_terrain_file(asset.source, source, error))
    {
        return false;
    }
    if (!asset.subresource_key.empty())
    {
        if (asset.subresource_key.starts_with("regions/"))
        {
            TerrainRegionKey key;
            return terrain_region_key(asset.subresource_key, key);
        }
        if (asset.subresource_key == "surface")
        {
            const auto root =
                asset.path.substr(0, asset.path.size() - asset.subresource_key.size());
            for (auto path : {root + "layer-base", root + "layer-normal", root + "layer-orm",
                              root + "layer-palette", std::string("builtin://shaders/terrain-pbr")})
            {
                Guid guid;
                if (!context.identities.path_to_guid(path, guid))
                {
                    error = "Missing terrain surface identity";
                    return false;
                }
                asset.build_dependencies.push_back(guid);
                asset.runtime_dependencies.push_back(std::move(guid));
            }
            return true;
        }
        if (!asset.subresource_key.starts_with("layer-"))
        {
            error = "Unknown terrain subresource";
            return false;
        }
        for (const auto& guid : source.recipe->material_layers)
        {
            if (!reference(guid, schema::AssetKind::material, context, asset.build_dependencies,
                           error))
            {
                return false;
            }
        }
        return true;
    }
    std::vector<TerrainRegionKey> keys;
    if (!terrain_source_keys(source, keys, error) ||
        !identities(asset, context, keys, asset.build_dependencies, error))
    {
        return false;
    }
    if (!source.recipe->material_layers.empty())
    {
        Guid surface;
        if (!context.identities.path_to_guid(asset.path + "/surface", surface))
        {
            error = "Missing terrain surface";
            return false;
        }
        asset.build_dependencies.push_back(surface);
        asset.runtime_dependencies.push_back(surface);
    }
    return true;
}
bool TerrainImporter::convert(const ImportAsset& asset, const ImportContext& context,
                              std::string& error) const
{
    ENGINE_PROFILE_FUNCTION();
    schema::TerrainSourceDataT source;
    if (!read_terrain_file(asset.source, source, error))
    {
        return false;
    }
    flatbuffers::FlatBufferBuilder builder;
    if (!asset.subresource_key.empty())
    {
        if (!asset.subresource_key.starts_with("regions/"))
        {
            return build_terrain_surface(asset, context, *source.recipe, error);
        }
        TerrainGenerationInput input;
        input.recipe = *source.recipe;
        input.source_revision = source.revision;
        for (const auto& edit : source.edits)
        {
            input.recipe.stamps.push_back(std::make_unique<schema::TerrainBrushDataT>(*edit));
        }
        if (!terrain_region_key(asset.subresource_key, input.key))
        {
            error = "Invalid terrain subresource key";
            return false;
        }
        schema::TerrainBlockDataT block;
        if (!generator_ || !generate_terrain_block(
                               input, *generator_,
                               context.project / "cache/terrain-authoring/blocks", block, error))
        {
            return false;
        }
        schema::FinishTerrainBlockDataBuffer(builder,
                                             schema::TerrainBlockData::Pack(builder, &block));
    }
    else
    {
        schema::TerrainAssetDataT data;
        const auto& recipe = *source.recipe;
        data.origin = recipe.origin;
        data.bounds = recipe.bounds;
        data.voxel_size = recipe.voxel_size;
        data.block_cells = recipe.block_cells;
        if (!recipe.material_layers.empty() &&
            !context.identities.path_to_guid(asset.path + "/surface", data.surface_material))
        {
            error = "Missing terrain surface identity";
            return false;
        }
        std::vector<TerrainRegionKey> keys;
        std::vector<Guid> guids;
        if (!terrain_source_keys(source, keys, error) ||
            !identities(asset, context, keys, guids, error))
        {
            return false;
        }
        for (const auto& path : recipe.material_layers)
        {
            Guid guid;
            if (!context.identities.path_to_guid(path, guid) && !GuidMap::valid_guid(path))
            {
                error = "Unknown terrain layer material";
                return false;
            }
            data.material_layers.push_back(guid.empty() ? path : guid);
        }
        for (std::size_t i = 0; i < keys.size(); ++i)
        {
            if (context.cancel_requested && context.cancel_requested())
            {
                error = "Terrain import cancelled";
                return false;
            }
            ByteBuffer bytes;
            if (!payload(context, guids[i], bytes, error))
            {
                return false;
            }
            flatbuffers::Verifier verifier(reinterpret_cast<const std::uint8_t*>(bytes.data()),
                                           bytes.size());
            if (!schema::VerifyTerrainBlockDataBuffer(verifier))
            {
                error = "Invalid generated terrain block";
                return false;
            }
            auto block = std::unique_ptr<schema::TerrainBlockDataT>(
                schema::GetTerrainBlockData(bytes.data())->UnPack());
            auto region = std::make_unique<schema::TerrainRegionEntryT>();
            region->key = keys[i];
            region->uniform = schema::TerrainUniformState::mixed;
            const schema::TerrainLayerSample default_layer{0, 0, 0, 0, Vec4{1, 0, 0, 0}};
            const bool default_layers =
                std::all_of(block->layers.begin(), block->layers.end(),
                            [&](const auto& v)
                            {
                                ENGINE_PROFILE_FUNCTION_PROBE();
                                return v == default_layer;
                            });
            const bool solid = std::all_of(block->density.begin(), block->density.end(),
                                           [&](float v)
                                           {
                                               ENGINE_PROFILE_FUNCTION_PROBE();
                                               return v == -float(recipe.voxel_size);
                                           });
            const bool air = std::all_of(block->density.begin(), block->density.end(),
                                         [&](float v)
                                         {
                                             ENGINE_PROFILE_FUNCTION_PROBE();
                                             return v == float(recipe.voxel_size);
                                         });
            if (default_layers && (solid || air))
            {
                region->uniform =
                    solid ? schema::TerrainUniformState::solid : schema::TerrainUniformState::air;
            }
            else
            {
                ByteBuffer entity;
                std::uint32_t header = 0;
                if (!ResourceSystem::encode_resource(guids[i], schema::AssetKind::terrain_block,
                                                     bytes, entity, header))
                {
                    error = "Cannot encode terrain block identity";
                    return false;
                }
                region->payload = guids[i];
                region->content_hash = hash_bytes(entity);
            }
            data.regions.push_back(std::move(region));
        }
        if (!TerrainVolume::validate(data, error))
        {
            return false;
        }
        schema::FinishTerrainAssetDataBuffer(builder,
                                             schema::TerrainAssetData::Pack(builder, &data));
    }
    return write_bytes(context.output / asset.guid,
                       std::as_bytes(std::span(builder.GetBufferPointer(), builder.GetSize())),
                       error);
}
bool TerrainImporter::finalize_dependencies(ImportAsset& asset, const ImportContext& context,
                                            std::string& error) const
{
    ENGINE_PROFILE_FUNCTION();
    asset.stream_dependencies.clear();
    if (!asset.subresource_key.empty())
    {
        return true;
    }
    ByteBuffer bytes;
    if (!payload(context, asset.guid, bytes, error))
    {
        return false;
    }
    flatbuffers::Verifier verifier(reinterpret_cast<const std::uint8_t*>(bytes.data()),
                                   bytes.size());
    if (!schema::VerifyTerrainAssetDataBuffer(verifier))
    {
        error = "Invalid generated terrain index";
        return false;
    }
    const auto* data = schema::GetTerrainAssetData(bytes.data());
    for (const auto* region : *data->regions())
    {
        if (region->uniform() == schema::TerrainUniformState::mixed)
        {
            asset.stream_dependencies.push_back(region->payload()->str());
        }
    }
    std::sort(asset.stream_dependencies.begin(), asset.stream_dependencies.end());
    return true;
}
} // namespace engine::plugins::terrain
