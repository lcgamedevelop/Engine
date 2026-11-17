#include "plugins/runtime/terrain/codecs/terrain_block_resource_codec.hpp"
#include "plugins/runtime/terrain/codecs/terrain_resource_codec.hpp"
#include "plugins/runtime/terrain/terrain_component_runtime.hpp"
#include "plugins/runtime/terrain/terrain_mesher.hpp"
#include "plugins/runtime/terrain/terrain_regions.hpp"
#include "plugins/runtime/terrain/terrain_samples.hpp"
#include "plugins/runtime/terrain/terrain_system.hpp"
#include "resource_fixture.hpp"
#include "resource_services.hpp"
#include "runtime/app/app.hpp"
#include "runtime/app/runtime_host.hpp"
#include "runtime/scene/camera.hpp"
#include "runtime/scene/ecs_storage.hpp"
#include "runtime/scene/environment_component.hpp"
#include "runtime/scene/scene.hpp"
#include "runtime/scene/transform.hpp"
#include "runtime_settings_fixture.hpp"
#include <thread>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
using namespace engine;
using namespace engine::plugins::terrain;

bool expect(bool value, const char* message)
{
    if (!value)
    {
        std::fprintf(stderr, "terrain: %s\n", message);
    }
    return value;
}

struct Fixture
{
    schema::TerrainAssetDataT source;
    std::vector<schema::TerrainBlockDataT> blocks;
    Fixture()
    {
        source.bounds = {{-4096, -256, -4096}, {4096, 256, 4096}};
        source.block_cells = 8;
        std::uint64_t identity = 1;
        // Sparse authored neighborhood in an 8 km world. Omitted regions remain unknown.
        for (int lod = 0; lod <= 2; ++lod)
        {
            const int range = 4 >> lod;
            const double scale = double(1 << lod);
            for (int x = -range; x < range; ++x)
            {
                for (int y = -range; y < range; ++y)
                {
                    for (int z = -range; z < range; ++z)
                    {
                        auto region = std::make_unique<schema::TerrainRegionEntryT>();
                        region->key = {x, y, z, static_cast<std::uint8_t>(lod)};
                        region->uniform = y < 0   ? schema::TerrainUniformState::solid
                                          : y > 0 ? schema::TerrainUniformState::air
                                                  : schema::TerrainUniformState::mixed;
                        if (y == 0)
                        {
                            char guid[37];
                            std::snprintf(guid, sizeof(guid), "11111111-2222-4333-8444-%012llx",
                                          static_cast<unsigned long long>(identity++));
                            region->payload = guid;
                            region->content_hash.assign(16, 'a');
                            schema::TerrainBlockDataT block;
                            block.key = region->key;
                            for (int pz = 0; pz < 8; ++pz)
                            {
                                for (int py = 0; py < 8; ++py)
                                {
                                    for (int px = 0; px < 8; ++px)
                                    {
                                        const double wx = (x * 8 + px) * scale;
                                        const double wz = (z * 8 + pz) * scale;
                                        const double height =
                                            3.5 + std::cos(wx * 0.3) * std::cos(wz * 0.2) * 0.5;
                                        block.density.push_back(
                                            static_cast<float>((y * 8 + py) * scale - height));
                                        block.layers.emplace_back(0, 0, 0, 0, Vec4{1, 0, 0, 0});
                                    }
                                }
                            }
                            blocks.push_back(std::move(block));
                        }
                        source.regions.push_back(std::move(region));
                    }
                }
            }
        }
    }
};

bool test_regions()
{
    Fixture fixture;
    TerrainVolume volume;
    std::string error;
    TerrainRegionQuery query;
    query.bounds = {{-8, -8, -8}, {8, 8, 8}};
    TerrainReadSnapshot snapshot;
    TerrainDelta residency;
    if (!expect(volume.initialize(fixture.source, error),
                "large bounds require only sparse index storage") ||
        !expect(!volume.read(query, snapshot) && !snapshot.source,
                "unloaded mixed regions are not air") ||
        !volume.stage_regions(fixture.blocks, {}, residency, error) ||
        !volume.validate_delta(residency))
    {
        std::fprintf(stderr, "%s\n", error.c_str());
        return false;
    }
    volume.publish(std::move(residency));
    if (!volume.read(query, snapshot))
    {
        return false;
    }
    float before = 0;
    if (!expect(terrain_detail::sample(snapshot, -1, -1, -1, 0, before) && before < 0,
                "negative sample coordinates use floor ownership"))
    {
        return false;
    }
    TerrainEditSpec edit;
    edit.expected_revision = snapshot.source_revision;
    edit.brushes.emplace_back();
    edit.brushes.front().center = {0, 0, 0};
    TerrainDelta delta;
    if (!TerrainVolume::stage_edit(snapshot, edit, delta, error) ||
        !expect(delta.changed.size() > 1 && volume.validate_delta(delta),
                "one brush stages every crossed block") ||
        !expect(terrain_detail::sample(snapshot, -1, -1, -1, 0, before) && before < 0,
                "editing never mutates the retained immutable snapshot"))
    {
        std::fprintf(stderr, "%s\n", error.c_str());
        return false;
    }
    TerrainDelta stale;
    if (!TerrainVolume::stage_edit(snapshot, edit, stale, error))
    {
        return false;
    }
    volume.publish(std::move(delta));
    TerrainReadSnapshot edited;
    if (!volume.read(query, edited) ||
        !expect(!volume.validate_delta(stale),
                "a second publication cannot overwrite a newer version") ||
        !expect(terrain_detail::sample(edited, -1, -1, -1, 0, before) && before > 0,
                "cross-boundary excavation publishes a consistent void"))
    {
        return false;
    }
    TerrainRegionQuery unknown;
    unknown.bounds = {{200, 0, 200}, {208, 8, 208}};
    if (!expect(!volume.read(unknown, edited),
                "unknown authored regions cannot become support-free air"))
    {
        return false;
    }
    auto bad = fixture.blocks.front();
    bad.density.pop_back();
    return expect(!volume.stage_regions(std::span(&bad, 1), {}, delta, error) && !delta.source,
                  "malformed region batches preserve the active volume and clear their output");
}

bool test_meshing_and_planning()
{
    Fixture fixture;
    TerrainVolume volume;
    std::string error;
    TerrainDelta delta;
    if (!volume.initialize(fixture.source, error) ||
        !volume.stage_regions(fixture.blocks, {}, delta, error))
    {
        return false;
    }
    volume.publish(std::move(delta));
    TerrainReadSnapshot snapshot;
    TerrainRegionQuery query;
    query.bounds = {{-8, 0, 0}, {0, 8, 8}};
    if (!volume.read(query, snapshot))
    {
        return false;
    }
    auto mesher = create_terrain_mesher(error);
    TerrainMeshInput input;
    input.samples = snapshot;
    input.key = {-1, 0, 0, 0};
    input.collision_required = true;
    TerrainMeshOutput plain, transition;
    if (!mesher || !mesher->build(input, plain, error) ||
        !expect(plain.mesh && !plain.mesh->indices.empty() &&
                    plain.collision.parts.front() == plain.mesh,
                "display and collision share one immutable regular-cell result"))
    {
        return false;
    }
    input.transition_faces = 2;
    if (!mesher->build(input, transition, error) ||
        !expect(transition.mesh->indices.size() > plain.mesh->indices.size(),
                "coarse neighbors add real transition-cell topology") ||
        !expect(transition.collision.parts.front()->indices == plain.mesh->indices,
                "visual LOD seams do not replace authoritative collision topology"))
    {
        return false;
    }
    auto cancel = std::make_shared<std::atomic<bool>>(true);
    auto layered_source = std::make_shared<schema::TerrainAssetDataT>(*snapshot.source);
    layered_source->material_layers = {"00000000-0000-4000-8000-000000000003",
                                       "00000000-0000-4000-8000-000000000003"};
    auto layered = input;
    layered.samples.source = layered_source;
    layered.transition_faces = 0;
    for (auto& [key, block] : layered.samples.blocks)
    {
        auto copy = std::make_shared<schema::TerrainBlockDataT>(*block);
        std::fill(copy->layers.begin(), copy->layers.end(),
                  schema::TerrainLayerSample{0, 1, 0, 0, {.25, .75, 0, 0}});
        block = std::move(copy);
    }
    TerrainMeshOutput painted;
    if (!mesher->build(layered, painted, error) ||
        !expect(
            painted.mesh->colors.size() == painted.mesh->vertices.size() &&
                std::any_of(painted.mesh->colors.begin(), painted.mesh->colors.end(),
                            [](const auto& c)
                            { return c.r() > 0 && c.r() < 1 && c.g() > 0 && c.g() < 1; }),
            "terrain triangles retain interpolated layer weights instead of one dominant material"))
    {
        return false;
    }
    input.cancel_requested = cancel;
    if (!expect(!mesher->build(input, transition, error) && !transition.mesh,
                "cancelled extraction returns no publishable geometry"))
    {
        return false;
    }
    TerrainPlanInput planning;
    planning.source = snapshot.source;
    TerrainView view;
    view.center = {0, 4, 0};
    view.visual_radius = 10;
    view.collision_radius = 8;
    planning.views.push_back(view);
    auto planner = create_terrain_region_planner(error);
    TerrainRegionPlan plan;
    if (!planner || !planner->plan(planning, plan, error) ||
        !expect(!plan.acquire.empty() && plan.acquire.size() < 256,
                "view planning is bounded independently of world size"))
    {
        std::fprintf(stderr, "%s\n", error.c_str());
        return false;
    }
    planning.budget.max_regions = 1;
    return expect(!planner->plan(planning, plan, error) && plan.acquire.empty(),
                  "insufficient region budget rejects a plan without returning partial coverage");
}

TerrainSystem* installed_terrain = nullptr;
bool install(RuntimeStage stage, WorldSystem& world, WindowSystem&, PhysicsSystem& physics,
             NavigationSystem&, RenderSystem& renderer, TaskSystem& tasks,
             std::vector<ScenePreparation*>& providers)
{
    if (stage != RuntimeStage::before_physics)
    {
        return true;
    }
    std::string error;
    auto system = std::make_unique<TerrainSystem>(world, *world.resource_loader(), tasks, renderer,
                                                  physics, create_terrain_region_planner(error),
                                                  create_terrain_mesher(error));
    installed_terrain = system.get();
    providers.push_back(system.get());
    std::unique_ptr<System> owned = std::move(system);
    return world.add_subsystem(owned);
}
bool test_runtime()
{
    Fixture data;
    test_support::ResourceFixture files;
    test_support::ResourceServices services;
    RuntimeSettings settings;
    if (!test_support::load_runtime_settings(settings))
    {
        return false;
    }
    settings.engine.width = 320;
    settings.engine.height = 240;
    settings.engine.window_title = "Terrain contract test";
    for (const auto& block : data.blocks)
    {
        if (!files.add("terrain/" + std::to_string(files.catalog.entries.size()), block))
        {
            return false;
        }
        for (auto& region : data.source.regions)
        {
            if (region->key == block.key)
            {
                region->payload = files.catalog.entries.back()->guid;
                region->content_hash = files.catalog.entries.back()->content_hash;
            }
        }
    }
    if (!files.add("test.terrain", data.source))
    {
        return false;
    }
    const auto guid = files.catalog.entries.back()->guid;
    AppConfig app;
    app.application_name = "engine-terrain-test";
    app.assets_path = files.root / "assets";
    app.persistent_data_path = files.root / "persistent";
    app.cache_path = files.root / "cache";
    if (!App::initialize(app))
    {
        return false;
    }
    RuntimeHost host;
    if (!services.core.add<TerrainResourceCodec>() ||
        !services.core.add<TerrainBlockResourceCodec>() || !services.initialize(settings.engine) ||
        !files.publish(services.core) || !services.prepare_builtins() ||
        !host.initialize(settings.engine, &services.core, &services.loader, nullptr, install,
                         &services.threads, settings.parallel))
    {
        std::fprintf(stderr, "terrain runtime initialize failed: %.*s\n",
                     int(services.core.last_error().size()), services.core.last_error().data());
        return false;
    }
    auto* scene = host.world()->create_scene("Sparse terrain");
    auto owner = scene->create_node("Terrain");
    auto component = owner.add_component<TerrainComponent>();
    auto camera_node = scene->create_node("Camera");
    auto camera = camera_node.add_component<Camera>();
    TransformSettings camera_pose;
    camera_pose.position = {0, 12, 20};
    camera_pose.rotation = {-25, 0, 0};
    schema::TerrainDataT config;
    config.terrain_guid = guid;
    config.streaming_enabled = true;
    config.collision_enabled = true;
    config.rvt_role = schema::RvtRole::writer_reader;
    auto environment = scene->create_node("Environment").add_component<EnvironmentComponent>();
    EnvironmentSettings environment_settings;
    environment_settings.ambient_intensity = .6F;
    environment_settings.rvt = std::make_unique<schema::RvtSettingsT>();
    environment_settings.rvt->enabled = true;
    environment_settings.rvt->bounds = {{-16, -16, -16}, {16, 16, 16}};
    environment_settings.rvt->virtual_resolution = 256;
    environment_settings.rvt->page_size = 64;
    environment_settings.rvt->physical_pages = 32;
    environment_settings.rvt->pages_per_frame = 4;
    if (!environment.set_settings(environment_settings))
    {
        return false;
    }
    config.initial_view = std::make_unique<schema::TerrainViewDataT>();
    config.initial_view->center = {0, 4, 0};
    config.initial_view->visual_radius = 10;
    config.initial_view->collision_radius = 8;
    if (!component || !component.set_settings(config) || !scene->set_active_camera(camera) ||
        !camera_node.get_component<Transform>().set_settings(camera_pose))
    {
        return false;
    }
    TerrainRegionQuery query;
    SceneScopeId inactive;
    if (!host.world()->begin_scope(SceneLoadKind::append_batch, scene->id(), inactive) ||
        !host.world()->with_scope_write(
            inactive,
            [&](Scene& candidate)
            {
                auto node = candidate.create_node("Inactive terrain");
                auto terrain = node.add_component<TerrainComponent>();
                auto parent = candidate.create_node("Inactive parent");
                auto child = candidate.create_node("Inactive descendant");
                auto descendant = child.add_component<TerrainComponent>();
                return terrain && descendant && terrain.set_settings(config) &&
                       descendant.set_settings(config) && child.set_parent(parent) &&
                       parent.set_enabled(false) && node.set_enabled(false);
            }) ||
        !expect(installed_terrain->prepare_scope(inactive, {}) == SceneWorkState::ready,
                "disabled terrain nodes and descendants do not block scene preparation"))
    {
        return false;
    }
    installed_terrain->release_scope(inactive);
    host.world()->release_scope(inactive);
    query.bounds = {{-8, 0, -8}, {8, 8, 8}};
    TerrainRegionSnapshot snapshot;
    const auto advance = [&](const auto& ready)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (!services.tick() || host.tick(0) != FrameStatus::running)
            {
                return false;
            }
            if (ready())
            {
                return true;
            }
            const auto* runtime = ecs_detail::component_runtime(component);
            if (!runtime->error_.empty())
            {
                std::fprintf(stderr, "terrain update: %s\n", runtime->error_.c_str());
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    };
    if (!expect(
            advance([&]()
                    { return component.query_regions(query, snapshot) && snapshot.revision > 0; }),
            "runtime loads and publishes only requested regions"))
    {
        return false;
    }
    RvtStatus rvt;
    if (!expect(advance(
                    [&]()
                    {
                        return host.renderer()->rvt_status(rvt) && rvt.effective &&
                               rvt.required_pages > 0 && rvt.ready_pages == rvt.required_pages;
                    }),
                "GPU pages become resident before scene sampling"))
    {
        std::fprintf(
            stderr, "RVT: %s (%u/%u), requested=%d supported=%d effective=%d demand=%llu\n",
            rvt.reason.c_str(), rvt.ready_pages, rvt.required_pages, rvt.requested, rvt.supported,
            rvt.effective, static_cast<unsigned long long>(rvt.demand_revision));
        return false;
    }
    const auto cached = rvt.cache_revision;
    if (!camera.set_enabled(false) || host.tick(0) != FrameStatus::running ||
        !host.renderer()->rvt_status(rvt) ||
        !expect(!rvt.effective, "a frame without a scene camera cannot report effective RVT") ||
        !camera.set_enabled(true) ||
        !advance([&]() { return host.renderer()->rvt_status(rvt) && rvt.effective; }))
    {
        return false;
    }
    auto insufficient = config;
    insufficient.budget = std::make_unique<schema::TerrainBudgetDataT>();
    insufficient.budget->max_regions = 1;
    if (!component.set_settings(insufficient) ||
        !advance(
            [&]()
            {
                return component.query_regions(query, snapshot) &&
                       snapshot.state == SceneWorkState::failed;
            }) ||
        !expect(!snapshot.error_code.empty() && !snapshot.error.empty() &&
                    snapshot.settings_revision > snapshot.published_settings_revision &&
                    snapshot.revision > 0 && !snapshot.regions.empty(),
                "streaming failure exposes the target error while retaining published regions") ||
        !component.set_settings(config) ||
        !advance(
            [&]()
            {
                return component.query_regions(query, snapshot) &&
                       snapshot.state == SceneWorkState::ready;
            }) ||
        !expect(snapshot.error.empty() && snapshot.error_code.empty() &&
                    snapshot.settings_revision == snapshot.published_settings_revision &&
                    snapshot.demand_revision == snapshot.published_demand_revision,
                "a corrected demand recovers with matching published identities"))
    {
        return false;
    }
    const auto initial = snapshot.revision;
    TerrainEditSpec edit;
    edit.expected_revision = initial;
    edit.brushes.emplace_back();
    edit.brushes.back().center = {0, 3, 0};
    edit.brushes.back().extent = {2, 2, 2};
    TerrainOperationId operation, cancelled;
    if (!component.request_edit(edit, operation) || !component.request_edit(edit, cancelled) ||
        !component.cancel_operation(cancelled))
    {
        return false;
    }
    TerrainOperationSnapshot status;
    if (!expect(advance(
                    [&]()
                    {
                        return component.operation_status(operation, status) &&
                               (status.state == TerrainOperationState::completed ||
                                status.state == TerrainOperationState::failed);
                    }),
                "asynchronous brush reaches a terminal state") ||
        !expect(status.state == TerrainOperationState::completed, status.error.c_str()) ||
        !expect(status.published_revision > initial,
                "brush publishes a new matching density, mesh and collision revision"))
    {
        return false;
    }
    if (!expect(advance(
                    [&]()
                    {
                        return component.operation_status(cancelled, status) &&
                               status.state == TerrainOperationState::cancelled;
                    }),
                "queued cancellation drains without publication") ||
        !component.release_operation(operation) || !component.release_operation(cancelled))
    {
        return false;
    }
    if (!expect(advance(
                    [&]()
                    {
                        return host.renderer()->rvt_status(rvt) && rvt.effective &&
                               rvt.cache_revision > cached && rvt.ready_pages == rvt.required_pages;
                    }),
                "editing invalidates and regenerates matching GPU pages"))
    {
        return false;
    }
    environment_settings.rvt->enabled = false;
    if (!environment.set_settings(environment_settings) ||
        !advance(
            [&]()
            {
                return host.renderer()->rvt_status(rvt) && !rvt.requested && !rvt.effective &&
                       rvt.resident_bytes == 0;
            }))
    {
        return false;
    }
    environment_settings.rvt->enabled = true;
    if (!environment.set_settings(environment_settings) ||
        !advance(
            [&]()
            {
                return host.renderer()->rvt_status(rvt) && rvt.effective &&
                       rvt.ready_pages == rvt.required_pages;
            }))
    {
        return false;
    }
    TerrainOperationId save;
    if (!component.request_save({"roundtrip", "checkpoint-a", 0}, save) ||
        !advance(
            [&]()
            {
                return component.operation_status(save, status) &&
                       (status.state == TerrainOperationState::completed ||
                        status.state == TerrainOperationState::failed);
            }) ||
        !expect(status.state == TerrainOperationState::completed, status.error.c_str()))
    {
        return false;
    }
    if (!expect(status.stages.size() == 1 && status.stages.front().stage == "manifest" &&
                    status.stages.front().state == SceneWorkState::ready &&
                    status.stages.front().completed == status.stages.front().total,
                "completed save reports a completed manifest stage") ||
        !component.release_operation(save))
    {
        return false;
    }
    const auto density = [&]()
    {
        TerrainReadSnapshot saved;
        float value = -100;
        auto* runtime = ecs_detail::component_runtime(component);
        if (!runtime->volume_.read(query, saved) ||
            !terrain_detail::sample(saved, 0, 3, 0, 0, value))
        {
            return -100.F;
        }
        return value;
    };
    if (!expect(density() > 0, "saved excavation is present before reset"))
    {
        return false;
    }
    TerrainOperationId reset;
    if (!component.request_reset({query.bounds, 0}, reset) ||
        !advance([&]()
                 { return component.operation_status(reset, status) && terminal(status.state); }) ||
        !expect(status.state == TerrainOperationState::completed, status.error.c_str()) ||
        !expect(density() < 0, "reset restores the authored solid density") ||
        !component.release_operation(reset))
    {
        return false;
    }
    TerrainOperationId restored;
    if (!component.request_load({"roundtrip", "checkpoint-a", ""}, restored) ||
        !advance(
            [&]()
            { return component.operation_status(restored, status) && terminal(status.state); }) ||
        !expect(status.state == TerrainOperationState::completed, status.error.c_str()) ||
        !expect(density() > 0, "checkpoint restores edited density through normal streaming") ||
        !component.release_operation(restored))
    {
        return false;
    }
    host.shutdown();
    services.shutdown();
    App::shutdown();
    return true;
}
} // namespace
int main()
{
    return test_regions() && test_meshing_and_planning() && test_runtime() ? 0 : 1;
}
