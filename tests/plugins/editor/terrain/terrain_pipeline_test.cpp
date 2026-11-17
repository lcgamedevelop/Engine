#include "editor/asset_pipeline/asset_pipeline_system.hpp"
#include "editor/commands/editor_edit.hpp"
#include "editor/editor_environment.hpp"
#include "editor/project/project_system.hpp"
#include "plugins/editor/terrain/terrain_authoring_system.hpp"
#include "plugins/editor/terrain/terrain_codec.hpp"
#include "plugins/editor/terrain/terrain_generation_cache.hpp"
#include "plugins/editor/terrain/terrain_importer.hpp"
#include "plugins/editor/terrain/terrain_source.hpp"
#include "runtime/engine_config.hpp"
#include "runtime/resource/resource_system.hpp"
#include "runtime/resource/virtual_file_system.hpp"
#include "runtime/schema/terrain_generated.h"
#include "runtime/schema/texture_generated.h"
#include "runtime/task/task_system.hpp"
#include "runtime/task/thread_system.hpp"
#include "runtime/utils/path_utils.hpp"
#include <thread>

#include <flatbuffers/verifier.h>
#include <toml++/toml.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace
{
namespace fs = std::filesystem;
using namespace engine;
constexpr std::string_view terrain_guid = "70000000-0000-4000-8000-000000000001";
constexpr std::string_view scene_guid = "70000000-0000-4000-8000-000000000002";

bool expect(bool condition, std::string_view message)
{
    if (!condition)
    {
        std::fprintf(stderr, "Terrain pipeline: %.*s\n", static_cast<int>(message.size()),
                     message.data());
    }
    return condition;
}

bool write(const fs::path& path, std::string_view text)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    file.close();
    return static_cast<bool>(file);
}

std::string metadata(std::string_view guid, std::string_view importer)
{
    return "guid = \"" + std::string(guid) + "\"\nimporter = \"" + std::string(importer) +
           "\"\nmeta_version = 1\n";
}

std::string terrain_source(int seed)
{
    return "schema_version = 2\nbounds_min = [-8,-8,-8]\nbounds_max = [8,8,8]\n"
           "voxel_size = 1\nblock_cells = 8\nlod_count = 2\n"
           "height = 0\namplitude = 2\nfrequency = 0.2\nseed = " +
           std::to_string(seed) +
           "\n[[stamps]]\ncenter = [0,-3,0]\nextent = [2,2,2]\noperation = \"dig\"\n";
}

struct Project
{
    fs::path root;

    ~Project()
    {
        if (!root.empty())
        {
            std::error_code ignored;
            fs::remove_all(root, ignored);
        }
    }

    bool initialize()
    {
        std::error_code error;
        root = fs::temp_directory_path(error);
        if (error)
        {
            root.clear();
            return false;
        }
        root /= "engine-terrain-pipeline-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        for (const auto* directory : {"assets", "settings", "cache"})
        {
            fs::create_directories(root / directory, error);
            if (error)
            {
                return false;
            }
        }
        return write(root / "settings/editor-import.toml", "[common]\nimport_threads = 1\n");
    }
};

bool run(Project& project)
{
    const std::string scene = "name = \"Terrain test\"\nactive_camera = 0\n"
                              "[[nodes]]\nid = 1\nname = \"Terrain\"\nparent = 0\n"
                              "[[nodes.components]]\ntype = \"transform\"\n"
                              "[[nodes.components]]\ntype = \"terrain\"\nterrain_guid = \"" +
                              std::string(terrain_guid) +
                              "\"\nstreaming_enabled = true\ncollision_enabled = true\n";
    if (!expect(write(project.root / "assets/test.terrain", terrain_source(1)) &&
                    write(project.root / "assets/test.terrain.meta",
                          metadata(terrain_guid, "terrain")) &&
                    write(project.root / "assets/test.scene", scene) &&
                    write(project.root / "assets/test.scene.meta", metadata(scene_guid, "scene")),
                "write terrain source and scene fixture"))
    {
        return false;
    }
    engine::ThreadSystem threads;
    engine::TaskSystem tasks(threads, {"Editor", 4, engine::TaskPriority::low});
    editor::AssetPipelineSystem pipeline(tasks);
    std::string generator_error;
    std::unique_ptr<editor::asset_pipeline::AssetImporter> importer =
        std::make_unique<plugins::terrain::TerrainImporter>(
            plugins::terrain::create_terrain_generator(generator_error));
    if (!expect(plugins::terrain::register_terrain_codec() && threads.initialize({}) &&
                    tasks.initialize({}) && pipeline.initialize({}) &&
                    pipeline.add_importer(importer),
                "initialize terrain importer and component codec") ||
        !expect(pipeline.build_assets(project.root), pipeline.last_error()))
    {
        return false;
    }
    ResourceCatalog catalog;
    ByteBuffer bytes;
    if (!expect(catalog.assign(pipeline.catalog()) &&
                    ResourceSystem::read_resource(catalog, {{"content", project.root / "cache"}},
                                                  terrain_guid, bytes),
                "read generated standalone terrain resource"))
    {
        return false;
    }
    flatbuffers::Verifier verifier(reinterpret_cast<const std::uint8_t*>(bytes.data()),
                                   bytes.size());
    const auto* terrain_entry = catalog.find(terrain_guid);
    const auto* scene_entry = catalog.find(scene_guid);
    if (!expect(schema::VerifyTerrainAssetDataBuffer(verifier) && terrain_entry != nullptr &&
                    terrain_entry->kind == schema::AssetKind::terrain &&
                    terrain_entry->dependencies.empty() && scene_entry != nullptr &&
                    scene_entry->dependencies.size() == 2 &&
                    std::find(scene_entry->dependencies.begin(), scene_entry->dependencies.end(),
                              terrain_guid) != scene_entry->dependencies.end(),
                "terrain is independent and generic scene codec discovers its actual reference"))
    {
        return false;
    }
    const auto* volume = schema::GetTerrainAssetData(bytes.data());
    if (!expect(volume->schema_version() == 2 && volume->regions()->size() == 16 &&
                    !terrain_entry->stream_dependencies.empty(),
                "source produces a sparse index with independent streamed payloads") ||
        !expect(pipeline.build_assets(project.root), pipeline.last_error()) ||
        !expect(pipeline.last_build().imported == 0 && pipeline.last_build().reused >= 18,
                "unchanged source reuses the index and region GUID resources"))
    {
        return false;
    }
    const auto first_hash = terrain_entry->content_hash;
    if (!expect(write(project.root / "assets/test.terrain", terrain_source(2)) &&
                    pipeline.build_assets(project.root),
                "changed generator parameter rebuilds the terrain") ||
        !expect(catalog.assign(pipeline.catalog()) && catalog.find(terrain_guid) != nullptr &&
                    catalog.find(terrain_guid)->content_hash != first_hash,
                "generator changes content while retaining resource GUID"))
    {
        return false;
    }
    ByteBuffer published;
    ByteBuffer published_catalog;
    if (!expect(
            VirtualFileSystem::read_file(project.root / "cache/gen" / terrain_guid, published) &&
                VirtualFileSystem::read_file(project.root / "cache/catalog.bin", published_catalog),
            "snapshot valid published batch"))
    {
        return false;
    }
    const auto revision = pipeline.last_build().revision;
    for (const auto& invalid : {terrain_source(2) + "\nunknown = 1\n",
                                std::string("cells_x = 15\n"), std::string("seed = 1.5\n"),
                                std::string("[[cavities]]\ncenter = [0,0,0]\nradius = -1\n")})
    {
        ByteBuffer current;
        ByteBuffer current_catalog;
        if (!expect(
                write(project.root / "assets/test.terrain", invalid) &&
                    !pipeline.build_assets(project.root) &&
                    pipeline.last_build().revision == revision &&
                    VirtualFileSystem::read_file(project.root / "cache/gen" / terrain_guid,
                                                 current) &&
                    VirtualFileSystem::read_file(project.root / "cache/catalog.bin",
                                                 current_catalog) &&
                    current == published && current_catalog == published_catalog,
                "invalid generation input preserves the previously published volume and catalog"))
        {
            return false;
        }
    }
    plugins::terrain::TerrainCodec codec;
    auto parsed = toml::parse(
        "terrain_guid = \"70000000-0000-4000-8000-000000000001\"\n"
        "surface_material_guid = \"70000000-0000-4000-8000-000000000003\"\n"
        "streaming_enabled = true\ncollision_enabled = true\nrvt_role = \"writer_reader\"\n"
        "[initial_view]\ncenter = {x = 0, y = 0, z = 0}\nvisual_radius = 12\ncollision_radius = "
        "8\n");
    schema::ComponentDataT component;
    schema::ComponentDataT roundtrip;
    toml::table encoded;
    std::string error;
    if (!expect(parsed && codec.read(parsed.table(), component, error), "decode terrain component"))
    {
        return false;
    }
    codec.write(component, encoded);
    if (!expect(codec.read(encoded, roundtrip, error) && component == roundtrip,
                "component codec round trip retains GUIDs and settings"))
    {
        return false;
    }
    encoded.insert_or_assign("terrain_guid", "test.terrain");
    if (!expect(!codec.read(encoded, roundtrip, error),
                "scene persistence requires GUID identity rather than unresolved paths"))
    {
        return false;
    }
    pipeline.shutdown();
    return true;
}
bool generation_inputs()
{
    using namespace engine::plugins::terrain;
    std::string error;
    auto generator = create_terrain_generator(error);
    TerrainGenerationInput input;
    input.recipe.bounds = {{-8, -8, -8}, {8, 8, 8}};
    input.recipe.block_cells = 8;
    input.recipe.lod_count = 2;
    input.recipe.amplitude = 0;
    input.recipe.heightmap = std::make_unique<schema::TerrainHeightmapDataT>();
    auto& map = *input.recipe.heightmap;
    map.width = map.height = 2;
    map.samples = {0, 1, 0, 1};
    map.height_scale = 4;
    map.height_offset = -2;
    schema::TerrainBlockDataT block;
    if (!generator || !generator->generate(input, block, error) ||
        !expect(!block.uniform && block.density[0] == 0 && block.density[7] == -1,
                "heightmap bilinear samples produce the authoritative density"))
    {
        return false;
    }
    input.key = {0, 100, 0, 0};
    if (!generator->generate(input, block, error) ||
        !expect(block.uniform && block.density.size() == 1 && block.density.front() > 0,
                "known empty regions are stored as one uniform sample"))
    {
        return false;
    }
    input.key = {};
    input.recipe.noise_graph = "invalid";
    if (!expect(!generator->generate(input, block, error),
                "invalid noise graphs fail instead of silently using another generator"))
    {
        return false;
    }
    input.recipe.noise_graph.clear();
    input.recipe.heightmap.reset();
    input.recipe.amplitude = 0.5;
    input.recipe.frequency = 0.125;
    if (!generator->generate(input, block, error))
    {
        return false;
    }
    const auto [minimum, maximum] =
        std::minmax_element(block.density.begin(), block.density.begin() + 8);
    if (!expect(*maximum - *minimum > 0.15F,
                "recipe frequency is applied once rather than FastNoise default feature scale"))
    {
        return false;
    }
    schema::TerrainSourceDataT source;
    source.recipe = std::make_unique<schema::TerrainGenerationSpecT>(input.recipe);
    std::string text;
    schema::TerrainSourceDataT decoded;
    if (!write_terrain_source(source, text, error) || !read_terrain_source(text, decoded, error) ||
        !expect(source == decoded, "heightmap and recipe survive source serialization"))
    {
        return false;
    }
    source.recipe->bounds = {{-4000, -128, -4000}, {4000, 128, 4000}};
    for (const auto* field : {"seed = 1.5", "revision = false", "roads = 1", "material_layers = 1"})
    {
        const auto invalid = std::string("schema_version = 2\nbounds_min = [0,0,0]\n") +
                             "bounds_max = [8,8,8]\n" + field + "\n";
        if (!expect(!read_terrain_source(invalid, decoded, error),
                    "typed source fields reject invalid values rather than defaulting"))
        {
            return false;
        }
    }
    for (int i = 0; i != 34; ++i)
    {
        auto stamp = std::make_unique<schema::TerrainBrushDataT>();
        stamp->operation = schema::TerrainBrushOperation::smooth;
        stamp->center = {0, 0, 0};
        stamp->extent = {100, 100, 100};
        input.recipe.stamps.push_back(std::move(stamp));
    }
    if (!expect(!generator->generate(input, block, error) && !error.empty(),
                "deep smoothing stacks fail within a bounded call stack"))
    {
        return false;
    }
    source.recipe->voxel_size = 8;
    source.recipe->block_cells = 16;
    source.recipe->lod_count = 6;
    std::vector<TerrainRegionKey> keys;
    return expect(terrain_source_keys(source, keys, error) && keys.size() < 65536,
                  "the authored eight kilometer world uses a bounded sparse index");
}

bool layered_surface(Project& project)
{
    const auto source =
        std::string("material_layers = [\"70000000-0000-4000-8000-000000000010\"]\n") +
        terrain_source(2);
    if (!write(project.root / "assets/test.terrain", source) ||
        !write(project.root / "assets/layer.material",
               "shader = \"00000000-0000-4000-8000-000000000001\"\n[[parameters]]\nname = "
               "\"baseColor\"\ntype = \"v4\"\nvalue = [0.2,0.4,0.3,1]\n") ||
        !write(project.root / "assets/layer.material.meta",
               metadata("70000000-0000-4000-8000-000000000010", "material")))
    {
        return false;
    }
    ThreadSystem threads;
    TaskSystem tasks(threads);
    editor::AssetPipelineSystem pipeline(tasks);
    std::string generator_error;
    std::unique_ptr<editor::asset_pipeline::AssetImporter> importer =
        std::make_unique<plugins::terrain::TerrainImporter>(
            plugins::terrain::create_terrain_generator(generator_error));
    if (!threads.initialize({}) || !tasks.initialize({}) || !pipeline.initialize({}) ||
        !pipeline.add_importer(importer) ||
        !expect(pipeline.build_assets(project.root), pipeline.last_error()))
    {
        return false;
    }
    ResourceCatalog catalog;
    ByteBuffer bytes;
    if (!catalog.assign(pipeline.catalog()) ||
        !ResourceSystem::read_resource(catalog, {{"content", project.root / "cache"}}, terrain_guid,
                                       bytes))
    {
        return false;
    }
    const auto world = schema::UnPackTerrainAssetData(bytes.data());
    const auto* entry = catalog.find(terrain_guid);
    if (!expect(entry && entry->dependencies == std::vector<Guid>{world->surface_material},
                "world loads the compiled surface rather than duplicate source materials"))
    {
        return false;
    }
    Guid texture_guid;
    if (!catalog.identities().path_to_guid("test.terrain/layer-base", texture_guid) ||
        !ResourceSystem::read_resource(catalog, {{"content", project.root / "cache"}}, texture_guid,
                                       bytes))
    {
        return false;
    }
    const auto texture = schema::UnPackTextureData(bytes.data());
    return expect(texture->schema_version == 3 && texture->array_layers == 2 &&
                      texture->mip_count == 8 && texture->subresources.size() == 16 &&
                      texture->srgb,
                  "layer compiler produces complete PBR array layers and mips");
}

class OffsetGenerator final : public plugins::terrain::TerrainGenerator
{
public:
    OffsetGenerator(std::string_view identity, float offset,
                    std::unique_ptr<plugins::terrain::TerrainGenerator> base)
        : identity_(identity), offset_(offset), base_(std::move(base))
    {
    }
    std::string_view fingerprint() const noexcept override
    {
        return identity_;
    }
    bool generate(const plugins::terrain::TerrainGenerationInput& input,
                  schema::TerrainBlockDataT& output, std::string& error) const override
    {
        ++calls;
        if (!base_ || !base_->generate(input, output, error))
        {
            return false;
        }
        for (auto& value : output.density)
        {
            value = std::clamp(value + offset_, -float(input.recipe.voxel_size),
                               float(input.recipe.voxel_size));
        }
        return true;
    }
    mutable std::atomic<std::uint32_t> calls = 0;

private:
    std::string_view identity_;
    float offset_ = 0;
    std::unique_ptr<plugins::terrain::TerrainGenerator> base_;
};

bool local_generation(Project& project)
{
    using namespace plugins::terrain;
    std::string error;
    auto generator = create_terrain_generator(error);
    TerrainGenerationInput input;
    input.recipe.bounds = {{-64, -8, -64}, {64, 8, 64}};
    input.recipe.block_cells = 8;
    input.recipe.voxel_size = 1;
    input.recipe.amplitude = 0;
    const auto cache = project.root / "cache/local-generation";
    schema::TerrainBlockDataT original, far, near, reference;
    if (!generator || !generate_terrain_block(input, *generator, cache, original, error))
    {
        return false;
    }
    const auto count = [&]()
    {
        std::size_t files = 0;
        std::error_code ec;
        for (fs::recursive_directory_iterator it(cache, ec), end; !ec && it != end;
             it.increment(ec))
        {
            if (it->is_regular_file(ec))
            {
                ++files;
            }
        }
        return ec ? 0 : files;
    };
    const auto initial_files = count();
    auto brush = std::make_unique<schema::TerrainBrushDataT>();
    brush->center = {32, 0, 32};
    brush->extent = {2, 2, 2};
    input.recipe.stamps.push_back(std::move(brush));
    input.source_revision = 2;
    if (!generate_terrain_block(input, *generator, cache, far, error) ||
        !generator->generate(input, reference, error) ||
        !expect(far == original && far == reference && count() == initial_files,
                "distant edits reuse the same block and match whole-recipe evaluation"))
    {
        return false;
    }
    input.recipe.stamps.front()->center = {2, 0, 2};
    if (!generate_terrain_block(input, *generator, cache, near, error) ||
        !generator->generate(input, reference, error) ||
        !expect(near != original && near == reference && count() == initial_files + 1,
                "nearby edits prepare a new immutable block without replacing the previous one"))
    {
        return false;
    }
    OffsetGenerator alternate("test-offset-1", .25F, create_terrain_generator(error));
    schema::TerrainBlockDataT changed, reused;
    return generate_terrain_block(input, alternate, cache, changed, error) &&
           generate_terrain_block(input, alternate, cache, reused, error) &&
           expect(changed != near && reused == changed && alternate.calls == 1,
                  "different generators isolate cached blocks and reuse their own output");
}

bool generator_injection(Project& project)
{
    using namespace plugins::terrain;
    std::string error;
    if (!write(project.root / "assets/test.terrain", terrain_source(2)))
    {
        return false;
    }
    ThreadSystem threads;
    TaskSystem tasks(threads);
    editor::AssetPipelineSystem pipeline(tasks);
    auto implementation =
        std::make_unique<OffsetGenerator>("test-import-a", .25F, create_terrain_generator(error));
    auto* selected = implementation.get();
    std::unique_ptr<editor::asset_pipeline::AssetImporter> importer =
        std::make_unique<TerrainImporter>(std::move(implementation));
    ByteBuffer before, after;
    if (!threads.initialize({}) || !tasks.initialize({}) || !pipeline.initialize({}) ||
        !pipeline.add_importer(importer) || !pipeline.build_assets(project.root) ||
        !expect(selected->calls > 0, "the importer uses its injected generator") ||
        !VirtualFileSystem::read_file(project.root / "cache/catalog.bin", before))
    {
        return false;
    }
    const auto calls = selected->calls.load();
    if (!pipeline.build_assets(project.root) ||
        !expect(selected->calls == calls, "unchanged imports reuse selected generator artifacts"))
    {
        return false;
    }
    implementation =
        std::make_unique<OffsetGenerator>("test-import-b", -.25F, create_terrain_generator(error));
    selected = implementation.get();
    importer = std::make_unique<TerrainImporter>(std::move(implementation));
    return pipeline.set_importer(importer) && pipeline.build_assets(project.root) &&
           VirtualFileSystem::read_file(project.root / "cache/catalog.bin", after) &&
           expect(selected->calls > 0 && before != after,
                  "changing generator fingerprints rebuilds unchanged sources with new output");
}

bool authoring(Project& project)
{
    using namespace engine::plugins::terrain;
    editor::ProjectSystem project_system(PathUtils::to_utf8(project.root));
    editor::EditorEnvironment environment(project_system, project.root / "persistent");
    ThreadSystem threads;
    TaskSystem tasks(threads);
    std::string error;
    if (!project_system.initialize({}) || !environment.initialize() || !threads.initialize({}) ||
        !tasks.initialize({}))
    {
        return false;
    }
    TerrainAuthoringSystem system(environment, tasks, create_terrain_generator(error),
                                  create_terrain_mesher(error));
    if (!system.initialize({}))
    {
        return false;
    }
    TerrainAuthoringSpec spec;
    spec.path = "generated.terrain";
    if (!read_terrain_source(terrain_source(7), spec.source, error))
    {
        return false;
    }
    TerrainAuthoringId id, cancelled;
    TerrainAuthoringId repeated;
    auto cancelled_spec = spec;
    cancelled_spec.path = "cancelled.terrain";
    if (!system.start(spec, id) || !system.start(spec, repeated) ||
        !expect(id == repeated, "identical submissions reuse one job") ||
        !system.start(cancelled_spec, cancelled) || !system.cancel(cancelled))
    {
        return false;
    }
    TerrainAuthoringSnapshot status;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    do
    {
        if (!system.tick(0) || !system.status(id, status))
        {
            return false;
        }
        if (status.state == TerrainAuthoringState::ready_to_publish ||
            status.state == TerrainAuthoringState::failed)
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    if (!expect(status.state == TerrainAuthoringState::ready_to_publish, status.error) ||
        !expect(!fs::exists(project.root / "assets/generated.terrain"),
                "preparation does not publish source"))
    {
        return false;
    }
    std::unique_ptr<editor::EditorEdit> edit, duplicate;
    if (!system.make_edit(id, edit) ||
        !expect(!system.make_edit(id, duplicate), "a job issues one edit") ||
        !expect(edit->apply(error), error))
    {
        return false;
    }
    schema::TerrainSourceDataT source;
    if (!read_terrain_file(project.root / "assets/generated.terrain", source, error) ||
        !expect(source == spec.source, "published source retains the exact recipe"))
    {
        return false;
    }
    editor::AssetPipelineSystem pipeline(tasks);
    std::unique_ptr<editor::asset_pipeline::AssetImporter> importer =
        std::make_unique<TerrainImporter>(create_terrain_generator(error));
    // The prior invalid-source regression intentionally leaves test.terrain invalid.
    if (!write(project.root / "assets/test.terrain", terrain_source(2)) ||
        !pipeline.initialize({}) || !pipeline.add_importer(importer) ||
        !expect(pipeline.build_assets(project.root), pipeline.last_error()))
    {
        return false;
    }
    pipeline.shutdown();
    if (!expect(edit->revert(error), error) ||
        !expect(!fs::exists(project.root / "assets/generated.terrain"),
                "undo retracts the created source") ||
        !expect(edit->apply(error), error))
    {
        return false;
    }
    std::error_code file_error;
    const auto published_time =
        fs::last_write_time(project.root / "assets/generated.terrain", file_error);
    if (file_error)
    {
        return false;
    }
    TerrainAuthoringSystem recovered(environment, tasks, create_terrain_generator(error),
                                     create_terrain_mesher(error));
    TerrainAuthoringId recovered_id;
    if (!recovered.initialize({}) || !recovered.start(spec, recovered_id))
    {
        return false;
    }
    const auto recovery_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do
    {
        if (!recovered.tick(0) || !recovered.status(recovered_id, status))
        {
            return false;
        }
        if (status.state == TerrainAuthoringState::published ||
            status.state == TerrainAuthoringState::failed)
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < recovery_deadline);
    if (!expect(
            status.state == TerrainAuthoringState::published &&
                fs::last_write_time(project.root / "assets/generated.terrain", file_error) ==
                    published_time &&
                !file_error,
            "a new authoring session recognizes an already published recipe without rewriting it"))
    {
        return false;
    }
    recovered.shutdown();
    if (!write(project.root / "assets/generated.terrain", terrain_source(8)) ||
        !expect(!edit->revert(error), "undo refuses external source changes"))
    {
        return false;
    }
    if (!system.status(id, status) || status.state != TerrainAuthoringState::published ||
        !system.release(id) || !system.status(cancelled, status) ||
        status.state != TerrainAuthoringState::cancelled || !system.release(cancelled))
    {
        return false;
    }
    edit.reset();
    system.shutdown();
    tasks.shutdown();
    threads.shutdown();
    environment.shutdown();
    project_system.shutdown();
    return true;
}
} // namespace

int main()
{
    Project project;
    return generation_inputs() &&
                   expect(project.initialize(), "create temporary terrain project") &&
                   local_generation(project) && run(project) && generator_injection(project) &&
                   authoring(project) && layered_surface(project)
               ? 0
               : 1;
}
