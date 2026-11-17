#include "../../../support/render_fixture.hpp"
#include "resource_fixture.hpp"
#include "resource_services.hpp"
#include "scene_snapshot.hpp"

#include "plugins/editor/scene_bake/ibl_bake_job.hpp"
#include "plugins/editor/scene_bake/lighting_bake_job.hpp"
#include "runtime/engine_config.hpp"
#include "runtime/render/render_system.hpp"
#include "runtime/resource/codecs/texture_resource_codec.hpp"
#include "runtime/resource/types/mesh_resource.hpp"
#include "runtime/scene/camera.hpp"
#include "runtime/scene/environment_component.hpp"
#include "runtime/scene/light.hpp"
#include "runtime/scene/mesh_renderer.hpp"
#include "runtime/scene/scene.hpp"
#include "runtime/scene/transform.hpp"
#include "runtime/task/task_system.hpp"
#include "runtime/task/thread_system.hpp"
#include "runtime/window/window_system.hpp"
#include "runtime/world/world_system.hpp"

#include <SDL3/SDL_main.h>
#include <bgfx/bgfx.h>
#include <bx/math.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <numbers>
#include <string>

namespace
{
namespace bake = engine::plugins::scene_bake;

bool expect(bool value, const char* message, const std::string& error = {})
{
    if (!value)
    {
        std::fprintf(stderr, "scene bake core loader: %s: %s\n", message, error.c_str());
    }
    return value;
}

engine::schema::TextureDataT color_texture(std::string path, std::uint32_t width,
                                           std::uint32_t height, std::array<std::uint8_t, 4> color)
{
    engine::schema::TextureDataT result;
    result.schema_version = 3;
    result.path = std::move(path);
    result.width = width;
    result.height = height;
    result.encoding = engine::schema::TextureEncoding::native;
    result.format = engine::schema::TextureFormat::rgba8;
    result.subresources.emplace_back(0, 0, 0,
                                     static_cast<std::uint64_t>(result.width) * result.height * 4);
    result.srgb = false;
    for (std::uint32_t index = 0; index < width * height; ++index)
    {
        result.data.insert(result.data.end(), color.begin(), color.end());
    }
    return result;
}

double mean_red(const engine::schema::TextureDataT& texture)
{
    if (!engine::TextureResourceCodec::validate(texture) ||
        texture.encoding != engine::schema::TextureEncoding::native ||
        texture.format != engine::schema::TextureFormat::rgba16f || texture.subresources.empty())
    {
        return -1;
    }
    const std::size_t begin = texture.subresources.front().offset();
    const std::size_t pixels = std::size_t{texture.width} * texture.height;
    if (begin > texture.data.size() || pixels * 8 > texture.data.size() - begin)
    {
        return -1;
    }
    double sum = 0;
    for (std::size_t index = 0; index < pixels; ++index)
    {
        std::uint16_t red = 0;
        std::memcpy(&red, texture.data.data() + begin + index * 8, sizeof(red));
        const float value = bx::halfToFloat(red);
        if (!std::isfinite(value) || value < 0)
        {
            return -1;
        }
        sum += value;
    }
    return sum / pixels;
}

bool frame(engine::WindowSystem& windows, engine::RenderSystem& renderer)
{
    return windows.tick(1.0 / 60.0) && windows.can_render() && test_support::render_frame(renderer);
}

bool exercise()
{
    engine::EngineConfig config;
    config.width = config.height = 96;
    config.window_title = "Scene bake with the core single-resource loader";
    engine::ThreadSystem threads;
    engine::TaskSystem tasks(threads);
    test_support::ResourceServices services(&threads);
    auto& resources = services.core;
    engine::WorldSystem world(&services.loader);
    engine::WindowSystem windows;
    engine::RenderSystem renderer(windows, &world);
    test_support::ResourceFixture fixture;
    // Jobs must outlive the renderer shutdown so outstanding readback memory stays valid.
    bake::IblBakeJob ibl;
    bake::LightingBakeJob lighting(renderer, resources, tasks);
    const auto cleanup = [&]
    {
        lighting.shutdown();
        ibl.shutdown();
        renderer.shutdown();
        windows.shutdown();
        world.shutdown();
        resources.shutdown();
        tasks.shutdown();
        threads.shutdown();
    };
    // Keep cleanup on every early return without exposing any test-only production API.
    struct Cleanup
    {
        const decltype(cleanup)& run;
        ~Cleanup()
        {
            run();
        }
    } guard{cleanup};
    if (!expect(threads.initialize(config) && tasks.initialize(config) &&
                    (services.initialize(config) && services.prepare_builtins()) &&
                    world.initialize(config) && windows.initialize(config) &&
                    renderer.initialize(config),
                "initialize real renderer and the core loader") ||
        !expect(world.resource_loader() == &services.loader,
                "world borrows the explicitly injected resource loader"))
    {
        return false;
    }
    const auto panorama = color_texture("textures/white-panorama", 16, 8, {255, 255, 255, 255});
    const auto tint = color_texture("textures/red-albedo", 2, 2, {230, 20, 10, 255});
    engine::ResourceReference builtin_cube;
    if (!expect(services.load("builtin://meshes/cube", builtin_cube) &&
                    builtin_cube.data<engine::MeshResource>(),
                "load the standard multi-face cube fixture"))
    {
        return false;
    }
    engine::schema::MeshDataT geometry = *builtin_cube.data<engine::MeshResource>();
    geometry.path = "meshes/bake-cube";
    geometry.lightmap_texcoords = geometry.texcoords;
    const auto rebound_lightmap =
        color_texture("textures/rebinding-lightmap", 32, 32, {96, 96, 96, 255});
    engine::schema::MaterialDataT material;
    material.type_path = "materials/textured-bake";
    material.shader_path = "builtin://shaders/pbr";
    auto albedo = std::make_unique<engine::schema::MaterialTextureT>();
    albedo->name = "baseColorTexture";
    albedo->texture_path = tint.path;
    material.textures.push_back(std::move(albedo));
    if (!expect(fixture.add(panorama) && fixture.add(tint) && fixture.add(geometry) &&
                    fixture.add(rebound_lightmap) && fixture.add(material) &&
                    services.publish_and_load(fixture.catalog, {{"content", fixture.root}}),
                "publish formal GUID resources and a filesystem mount",
                std::string(resources.last_error())))
    {
        return false;
    }
    engine::ResourceReference source;
    if (!expect(services.load(panorama.path, source) && source &&
                    source.data<engine::TextureResource>() && source.handle().id != 0,
                "load an IBL input from its mounted entity without graph residency"))
    {
        return false;
    }
    std::string error;
    if (!expect(ibl.start(*source.data<engine::TextureResource>(), 16, 16, 32, error),
                "start GPU IBL bake", error))
    {
        return false;
    }
    source.reset();
    auto status = bake::BakeProgress::running;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (status == bake::BakeProgress::running && std::chrono::steady_clock::now() < deadline)
    {
        status = ibl.tick(renderer.frame_number(), error);
        if (!frame(windows, renderer))
        {
            return false;
        }
    }
    engine::schema::TextureDataT irradiance;
    irradiance.schema_version = 3;
    irradiance.path = "baked/irradiance";
    irradiance.width = irradiance.height = 16;
    irradiance.cube_map = true;
    irradiance.srgb = false;
    irradiance.format = engine::schema::TextureFormat::rgba16f;
    irradiance.encoding = engine::schema::TextureEncoding::native;
    const auto& ktx = ibl.outputs()[0];
    if (ktx.size() < 68)
    {
        return false;
    }
    irradiance.data.assign(ktx.begin() + 68, ktx.end());
    for (std::uint32_t face = 0; face < 6; ++face)
    {
        irradiance.subresources.emplace_back(0, face, face * 16 * 16 * 8, 16 * 16 * 8);
    }
    const double ibl_mean = mean_red(irradiance);
    if (!expect(status == bake::BakeProgress::complete && ibl.completed() == ibl.total() &&
                    std::abs(ibl_mean - 1) < 0.01 && !ibl.outputs()[1].empty() &&
                    !ibl.outputs()[2].empty(),
                "real GPU IBL readback preserves a white environment", error))
    {
        return false;
    }
    auto* scene = world.create_scene("Core loader bake fixture");
    if (!scene)
    {
        return false;
    }
    auto camera_node = scene->create_node("Camera");
    auto camera = camera_node.add_component<engine::Camera>();
    auto environment =
        scene->create_node("Environment").add_component<engine::EnvironmentComponent>();
    auto mesh_node = scene->create_node("Static textured cube");
    auto mesh = mesh_node.add_component<engine::MeshRenderer>();
    engine::TransformSettings eye;
    eye.position = {0, 0, 3};
    auto ambient = test_support::read_settings(environment);
    ambient.ambient_color = {1, 1, 1, 1};
    ambient.ambient_intensity = 1;
    if (!expect(camera && environment && mesh && scene->set_active_camera(camera) &&
                    camera_node.get_component<engine::Transform>().set_settings(eye) &&
                    environment.set_settings(ambient) && mesh.set_mesh(geometry.path) &&
                    mesh.material().set_type(material.type_path) && mesh.set_bake_static(true) &&
                    mesh.material().texture("baseColorTexture").id != 0 && frame(windows, renderer),
                "components retain mesh, material, shader and texture using single-item loads"))
    {
        return false;
    }
    if (!expect(mesh.set_lightmap(rebound_lightmap.path),
                "accept a CPU-loaded lightmap that has no GPU texture yet"))
    {
        return false;
    }
    engine::SceneWorkBudget budget;
    budget.items = 1;
    budget.upload_bytes = 256;
    bool saw_pending = false, rebind_ready = false;
    for (unsigned step = 0; step < 256 && !rebind_ready; ++step)
    {
        const auto preparation = renderer.prepare_scope({}, budget);
        if (!expect(preparation != engine::SceneWorkState::failed && windows.tick(1.0 / 60.0) &&
                        renderer.render() == engine::RenderSubmitStatus::submitted,
                    "pending lightmap GPU upload does not fail the scene frame"))
        {
            return false;
        }
        saw_pending = saw_pending || preparation == engine::SceneWorkState::working;
        rebind_ready = preparation == engine::SceneWorkState::ready;
    }
    if (!expect(saw_pending && rebind_ready && mesh.lightmap_resource().id != 0 &&
                    mesh.set_lightmap("") && frame(windows, renderer),
                "lightmap upload reaches ready and can be removed through normal preparation"))
    {
        return false;
    }
    engine::SceneCaptureRequest request;
    request.width = request.height = 32;
    bx::mtxLookAt(request.view.data(), {0, 0, 3}, {0, 0, 0}, {0, 1, 0}, bx::Handedness::Right);
    bx::mtxProj(request.projection.data(), 50, 1, 0.1F, 10, bgfx::getCaps()->homogeneousDepth,
                bx::Handedness::Right);
    request.node_ids.push_back(mesh_node.id());
    if (!expect(renderer.request_diffuse_capture(request), "queue textured diffuse capture"))
    {
        return false;
    }
    for (unsigned index = 0;
         index < 24 && renderer.diffuse_capture_status() == engine::SceneCaptureStatus::pending;
         ++index)
    {
        if (!frame(windows, renderer))
        {
            return false;
        }
    }
    engine::SceneCaptureImage captured;
    if (!expect(renderer.take_diffuse_capture(captured), "read back the actual textured scene"))
    {
        return false;
    }
    const auto center =
        (std::size_t{captured.height} / 2 * captured.width + captured.width / 2) * 4;
    if (!expect(captured.rgba[center] > captured.rgba[center + 1] * 3 &&
                    captured.rgba[center] > 0.01F,
                "capture uses the loaded red texture, not a missing/default material"))
    {
        return false;
    }
    for (unsigned repeat = 0; repeat < 3; ++repeat)
    {
        // Match the editor job order: take pixels, accept another request, render once,
        // then advance preparation. Cached target dimensions must not imply a live CPU buffer.
        if (!expect(renderer.request_diffuse_capture(request) && windows.tick(1.0 / 60.0) &&
                        renderer.render() == engine::RenderSubmitStatus::submitted &&
                        renderer.diffuse_capture_status() == engine::SceneCaptureStatus::pending,
                    "unprepared consecutive capture defers readback without discarding the frame"))
        {
            return false;
        }
        for (unsigned index = 0;
             index < 24 && renderer.diffuse_capture_status() == engine::SceneCaptureStatus::pending;
             ++index)
        {
            if (!frame(windows, renderer))
            {
                return false;
            }
        }
        if (!expect(renderer.take_diffuse_capture(captured) && captured.width == request.width &&
                        captured.height == request.height &&
                        captured.rgba.size() == std::size_t{request.width} * request.height * 4 &&
                        captured.rgba[center] > captured.rgba[center + 1] * 3 &&
                        captured.rgba[center] > 0.01F,
                    "each consecutive capture owns a prepared nonempty readback destination"))
        {
            return false;
        }
    }
    engine::schema::SceneDataT saved;
    auto sun_node = scene->create_node("Capture shadow sun");
    auto sun = sun_node.add_component<engine::Light>();
    auto sunlight = test_support::read_settings(sun);
    // Exercise real cascades without changing the white-furnace irradiance expectation.
    sunlight.intensity = 0;
    sunlight.cast_shadows = true;
    sunlight.shadow_resolution = 512;
    sunlight.shadow_cascades = 4;
    auto sun_pose = test_support::read_settings(sun_node.get_component<engine::Transform>());
    sun_pose.rotation = {-48, -25, 0};
    auto mesh_pose = test_support::read_settings(mesh_node.get_component<engine::Transform>());
    mesh_pose.position = {45, 0.01, 40};
    mesh_pose.scale = {6, 0.05, 6};
    if (!expect(sun.set_settings(sunlight) &&
                    sun_node.get_component<engine::Transform>().set_settings(sun_pose) &&
                    mesh_node.get_component<engine::Transform>().set_settings(mesh_pose) &&
                    scene->build_data(saved),
                "retain nonidentity multi-face geometry and capture shadow light"))
    {
        return false;
    }
    bake::LightingBakeSettings settings;
    settings.lightmap_resolution = 32;
    settings.hemisphere_resolution = 16;
    settings.bounce_count = 1;
    settings.interpolation_passes = 4;
    settings.source_fingerprint = "core-loader-fixture";
    const auto saved_sun =
        std::find_if(saved.nodes.begin(), saved.nodes.end(),
                     [](const auto& n)
                     {
                         return n && n->info && n->info->properties &&
                                n->info->properties->name == "Capture shadow sun";
                     });
    if (!expect(saved_sun != saved.nodes.end(), "resolve the saved shadow light identity"))
    {
        return false;
    }
    settings.light_node_ids.push_back((*saved_sun)->info->properties->id);
    if (!expect(lighting.start(saved, *scene, settings, error),
                "start lighting bake from saved identities with the core loader", error))
    {
        return false;
    }
    status = bake::BakeProgress::running;
    while (status == bake::BakeProgress::running && std::chrono::steady_clock::now() < deadline)
    {
        status = lighting.tick(error);
        if (!frame(windows, renderer))
        {
            return false;
        }
    }
    if (!expect(status == bake::BakeProgress::complete &&
                    lighting.completed() == lighting.total() &&
                    lighting.outputs().meshes.size() == 1,
                "complete UV2, renderer hemisphere captures and lightmapper integration", error))
    {
        return false;
    }
    const auto& baked = lighting.outputs().meshes.front();
    const double lighting_mean = baked.lightmap ? mean_red(*baked.lightmap) : -1;
    if (!expect(
            baked.mesh && baked.mesh->lightmap_texcoords.size() == baked.mesh->vertices.size() &&
                std::abs(lighting_mean - std::numbers::pi) < 0.03 && fixture.add(*baked.lightmap) &&
                services.publish_and_load(fixture.catalog, {{"content", fixture.root}}) &&
                services.load(baked.lightmap->path, source) && source &&
                source.data<engine::TextureResource>() &&
                mean_red(*source.data<engine::TextureResource>()) == lighting_mean,
            "load the baked lightmap entity again without a graph",
            std::string(resources.last_error())))
    {
        return false;
    }
    std::printf("scene bake: IBL=%.6f lightmap=%.6f expected=pi multi-face-captures=complete\n",
                ibl_mean, lighting_mean);
    return true;
}
} // namespace

int main(int, char**)
{
    return exercise() ? 0 : 1;
}
