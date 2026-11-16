#include "plugins/runtime/destruction/codecs/destruction_resource_codec.hpp"
#include "plugins/runtime/destruction/destruction_component.hpp"
#include "plugins/runtime/destruction/destruction_system.hpp"
#include "plugins/runtime/script/script_system.hpp"
#include "plugins/runtime/terrain/codecs/terrain_block_resource_codec.hpp"
#include "plugins/runtime/terrain/codecs/terrain_resource_codec.hpp"
#include "plugins/runtime/terrain/terrain_component.hpp"
#include "plugins/runtime/terrain/terrain_system.hpp"
#include "resource_fixture.hpp"
#include "runtime/app/app.hpp"
#include "runtime/app/runtime_host.hpp"
#include "runtime/engine_config.hpp"
#include "runtime/log/log_system.hpp"
#include "runtime/physics/physics_system.hpp"
#include "runtime/scene/scene.hpp"
#include "runtime/task/task_system.hpp"
#include "runtime/task/thread_system.hpp"
#include "runtime/world/world_system.hpp"
#include "runtime_settings_fixture.hpp"
#include "scene_snapshot.hpp"
#include "script_fixture.hpp"
#include "terrain_fixture.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace
{
using namespace engine;
using plugins::destruction::DestructionComponent;
using plugins::destruction::DestructionSystem;
using plugins::terrain::TerrainComponent;
using plugins::terrain::TerrainSystem;

std::string terrain_asset_guid;
PhysicsSystem* installed_physics = nullptr;
TerrainSystem* installed_terrain = nullptr;
DestructionSystem* installed_destruction = nullptr;
constexpr std::string_view destruction_guid = "50000000-0000-4000-8000-000000000022";
constexpr std::string_view source = R"js(
import { Gameplay, Scene, TerrainComponent, DestructionComponent } from 'engine';
function check(value, message) { if (!value) throw new Error(message); }
const bounds = {minimum:{x:-4,y:-4,z:-4},maximum:{x:4,y:4,z:4}};
const brush = {center:{x:0,y:-1,z:0},extent:{x:2,y:2,z:2},operation:'dig'};
export default class extends Gameplay {
    phase=0;
    callbacks=0;
    OnInitialize() {
        this.scene=Scene.Create('Terrain operations');
        this.terrain=this.scene.CreateNode('Terrain').AddComponent(TerrainComponent);
        this.building=this.scene.CreateNode('Building').AddComponent(DestructionComponent);
        check(this.terrain.RequestEdit({brushes:[brush]})===null,'unready request accepted');
        return true;
    }
    Update() {
        if(this.phase===0) {
            const regions=this.terrain.GetRegions({bounds});
            if(!regions || regions.revision===0n || !this.building.ready) return;
            check(this.terrain.RequestEdit({brushes:[]})===null,'empty brush batch accepted');
            const settings=this.terrain.GetSettings();
            check(settings.streamingEnabled && settings.collisionEnabled,'generated settings mismatch');
            this.revision=regions.revision;
            this.edit=this.terrain.RequestEdit({brushes:[brush],expectedRevision:this.revision});
            this.cancelled=this.terrain.RequestRetire({maxCount:1});
            check(this.edit && this.cancelled && this.terrain.CancelOperation(this.cancelled),'request identity or cancellation failed');
            check(this.terrain.GetOperation({...this.edit,componentInstance:0n})===null,'foreign operation identity accepted');
            check(this.building.DamageSphereAsync(.5,0,0,2,.25,0,success=>{check(success,'damage failed');++this.callbacks;}),'damage rejected');
            this.phase=1;
        } else if(this.phase===1) {
            const edit=this.terrain.GetOperation(this.edit),cancelled=this.terrain.GetOperation(this.cancelled);
            check(edit && cancelled && edit.state!=='failed', edit?.error || 'missing operation');
            if(edit.state!=='completed' || cancelled.state!=='cancelled' || this.callbacks!==1) return;
            check(edit.publishedRevision>this.revision && this.terrain.ReleaseOperation(this.edit) && this.terrain.ReleaseOperation(this.cancelled),'publication or release failed');
            check(this.terrain.GetOperation(this.edit)===null,'released operation remains accessible');
            const supports=this.terrain.CollectSupport({bounds});
            check(supports && supports.every(c=>c.IsValid()),'support set missing');
            check(this.building.RefreshSupport(supports,.2,.5),'cross-region support query failed');
            this.pending=this.terrain.RequestEdit({brushes:[brush]});
            check(this.pending && this.scene.Destroy(),'queue before scene destruction');
            this.scene=Scene.Create('Complete');
            check(this.terrain.GetOperation(this.pending)===null && this.terrain.RequestEdit({brushes:[brush]})===null,'stale owner accepted operations');
            this.phase=2;
        }
    }
}
)js";

bool add_resource(test_support::ResourceFixture& files, std::string_view guid,
                  std::string_view path, schema::AssetKind kind, ByteView payload)
{
    auto entry = std::make_unique<schema::AssetEntryT>();
    entry->guid = guid;
    entry->path = path;
    entry->kind = kind;
    if (kind == schema::AssetKind::script)
    {
        entry->module_path = path;
    }
    ByteBuffer bytes;
    if (!ResourceSystem::encode_resource(entry->guid, kind, payload, bytes, entry->header_size))
    {
        return false;
    }
    std::uint64_t hash = 14695981039346656037ULL;
    for (const auto value : bytes)
    {
        hash = (hash ^ std::to_integer<std::uint8_t>(value)) * 1099511628211ULL;
    }
    char digest[17]{};
    std::snprintf(digest, sizeof(digest), "%016llx", static_cast<unsigned long long>(hash));
    entry->content_hash = digest;
    entry->content_size = bytes.size();
    entry->location = std::make_unique<schema::FileLocationT>();
    entry->location->mount = "content";
    entry->location->path = "gen/" + entry->guid;
    entry->location->length = bytes.size();
    std::ofstream stream(files.root / entry->location->path, std::ios::binary);
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    stream.close();
    if (!stream)
    {
        return false;
    }
    files.catalog.entries.push_back(std::move(entry));
    return true;
}

bool add_fixtures(test_support::ResourceFixture& files)
{
    test_support::TerrainFixture fixture({{-8, -8, -8}, {8, 8, 8}}, 1);
    for (auto& block : fixture.blocks)
    {
        if (!files.add("terrain/" + std::to_string(files.catalog.entries.size()), block))
        {
            return false;
        }
        for (auto& region : fixture.source.regions)
        {
            if (region->key == block.key)
            {
                region->payload = files.catalog.entries.back()->guid;
                region->content_hash = files.catalog.entries.back()->content_hash;
            }
        }
    }
    if (!files.add("test.terrain", fixture.source))
    {
        return false;
    }
    terrain_asset_guid = files.catalog.entries.back()->guid;

    schema::DestructionAssetDataT structure;
    for (std::uint32_t index = 0; index < 2; ++index)
    {
        auto chunk = std::make_unique<schema::DestructionChunkDataT>();
        chunk->centroid = {static_cast<double>(index), 0, 0};
        chunk->anchored = true;
        chunk->mesh = std::make_unique<schema::MeshDataT>();
        for (const float z : {-0.5F, 0.5F})
        {
            for (const float y : {-0.5F, 0.5F})
            {
                for (const float x : {-0.5F, 0.5F})
                {
                    chunk->mesh->vertices.emplace_back(static_cast<float>(index) + x, y, z, 0, 1,
                                                       0);
                }
            }
        }
        chunk->mesh->indices = {0, 2, 1, 1, 2, 3, 4, 5, 6, 5, 7, 6, 0, 1, 4, 1, 5, 4,
                                2, 6, 3, 3, 6, 7, 0, 4, 2, 2, 4, 6, 1, 3, 5, 3, 7, 5};
        structure.chunks.push_back(std::move(chunk));
    }
    auto bond = std::make_unique<schema::DestructionBondDataT>();
    bond->chunk_a = 0;
    bond->chunk_b = 1;
    bond->centroid = {.5, 0, 0};
    bond->normal = {1, 0, 0};
    structure.bonds.push_back(std::move(bond));
    flatbuffers::FlatBufferBuilder destruction;
    schema::FinishDestructionAssetDataBuffer(
        destruction, schema::DestructionAssetData::Pack(destruction, &structure));
    return add_resource(files, destruction_guid, "test.destruction", schema::AssetKind::destruction,
                        {reinterpret_cast<const std::byte*>(destruction.GetBufferPointer()),
                         destruction.GetSize()}) &&
           add_resource(files, "50000000-0000-4000-8000-000000000023", "scripts/game.js",
                        schema::AssetKind::script,
                        {reinterpret_cast<const std::byte*>(source.data()), source.size()});
}

bool prepare_components(WorldSystem& world, TerrainSystem& terrain, DestructionSystem& destruction)
{
    auto* scene = world.scene();
    Node ground = {};
    Node building = {};
    if (scene)
    {
        for (const auto& node : test_support::scene_nodes(*scene))
        {
            if (node.name() == "Terrain")
            {
                ground = node;
            }
            if (node.name() == "Building")
            {
                building = node;
            }
        }
    }
    auto terrain_component = ground ? ground.get_component<TerrainComponent>() : TerrainComponent{};
    auto destruction_component =
        building ? building.get_component<DestructionComponent>() : DestructionComponent{};
    schema::TerrainDataT terrain_settings;
    terrain_settings.terrain_guid = terrain_asset_guid;
    terrain_settings.streaming_enabled = true;
    terrain_settings.collision_enabled = true;
    terrain_settings.initial_view = std::make_unique<schema::TerrainViewDataT>();
    terrain_settings.initial_view->visual_radius = 32;
    terrain_settings.initial_view->collision_radius = 32;
    schema::DestructionDataT destruction_settings;
    destruction_settings.destruction_guid = destruction_guid;
    return terrain_component && destruction_component &&
           terrain_component.set_settings(terrain_settings) &&
           destruction_component.set_settings(destruction_settings) && terrain.synchronize() &&
           destruction.synchronize();
}

bool install(RuntimeStage stage, WorldSystem& world, WindowSystem&, PhysicsSystem& physics,
             NavigationSystem&, RenderSystem& renderer, TaskSystem& tasks,
             std::vector<ScenePreparation*>& providers)
{
    if (stage != RuntimeStage::before_physics)
    {
        return true;
    }
    std::string error;
    auto terrain =
        std::make_unique<TerrainSystem>(world, *world.resource_loader(), tasks, renderer, physics,
                                        plugins::terrain::create_terrain_region_planner(error),
                                        plugins::terrain::create_terrain_mesher(error));
    installed_terrain = terrain.get();
    installed_physics = &physics;
    providers.push_back(terrain.get());
    std::unique_ptr<System> owned = std::move(terrain);
    if (!world.add_subsystem(owned))
    {
        return false;
    }
    auto destruction = std::make_unique<DestructionSystem>(world, physics, &tasks);
    installed_destruction = destruction.get();
    owned = std::move(destruction);
    return world.add_subsystem(owned);
}
bool run(test_support::ResourceFixture& files)
{
    test_support::ResourceServices services;
    RuntimeSettings settings;
    if (!test_support::load_runtime_settings(settings))
    {
        return false;
    }
    settings.engine.width = 320;
    settings.engine.height = 240;
    RuntimeHost host;
    if (!services.core.add<plugins::terrain::TerrainResourceCodec>() ||
        !services.core.add<plugins::terrain::TerrainBlockResourceCodec>() ||
        !services.core.add<plugins::destruction::DestructionResourceCodec>() ||
        !services.initialize(settings.engine) || !add_fixtures(files) ||
        !files.publish(services.core) || !services.prepare_builtins() ||
        !host.initialize(settings.engine, &services.core, &services.loader, nullptr, install,
                         &services.threads, settings.parallel))
    {
        return false;
    }
    plugins::script::ScriptSystem script(&services.loader, host.world(), host.renderer(), host.ui(),
                                         installed_physics);
    if (!script.initialize(settings.engine) ||
        !test_support::start_script(script, services, {{"content", files.root}},
                                    "scripts/game.js") ||
        !prepare_components(*host.world(), *installed_terrain, *installed_destruction))
    {
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    bool passed = false;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (!services.tick() || host.tick(0) != FrameStatus::running || !script.tick(.016) ||
            !script.status().running)
        {
            std::fprintf(stderr, "terrain script: %s\n", script.status().last_error.c_str());
            break;
        }
        if (host.world()->scene() && host.world()->scene()->name() == "Complete")
        {
            passed = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    script.stop_gameplay();
    script.shutdown();
    host.shutdown();
    services.shutdown();
    return passed;
}
} // namespace

int main()
{
    test_support::ResourceFixture files;
    AppConfig app;
    app.application_name = "engine-terrain-destruction-script-test";
    app.assets_path = files.root / "builtin";
    app.persistent_data_path = files.root / "persistent";
    app.cache_path = files.root / "cache";
    if (files.root.empty() || !App::initialize(app) || !LogSystem::instance().initialize({}))
    {
        App::shutdown();
        return 1;
    }
    const bool passed = run(files);
    LogSystem::instance().shutdown();
    App::shutdown();
    return passed ? 0 : 1;
}
