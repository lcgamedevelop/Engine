#include "../../support/render_fixture.hpp"
#include "resource_fixture.hpp"
#include "resource_services.hpp"
#include "scene_snapshot.hpp"

#include "plugins/runtime/resource_graph/resource_graph_system.hpp"
#include "runtime/engine_config.hpp"
#include "runtime/performance/profiling.hpp"
#include "runtime/render/render_constants.hpp"
#include "runtime/render/render_system.hpp"
#include "runtime/scene/camera.hpp"
#include "runtime/scene/environment_component.hpp"
#include "runtime/scene/light.hpp"
#include "runtime/scene/light_probe_volume_component.hpp"
#include "runtime/scene/mesh_renderer.hpp"
#include "runtime/scene/planar_reflection_component.hpp"
#include "runtime/scene/post_process_component.hpp"
#include "runtime/scene/scene.hpp"
#include "runtime/scene/transform.hpp"
#include "runtime/utils/path_utils.hpp"
#include "runtime/window/window_system.hpp"
#include "runtime/world/world_system.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <bgfx/bgfx.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <string_view>
#include <vector>

namespace
{
using namespace engine;

bool expect(bool value, const char* message)
{
    if (!value)
    {
        std::fprintf(stderr, "render submission: %s (SDL: %s)\n", message, SDL_GetError());
    }
    return value;
}

schema::MeshDataT grid()
{
    schema::MeshDataT mesh;
    mesh.path = "meshes/submission-grid";
    constexpr std::uint32_t width = 8;
    for (std::uint32_t y = 0; y <= width; ++y)
    {
        for (std::uint32_t x = 0; x <= width; ++x)
        {
            mesh.vertices.emplace_back(static_cast<float>(x) / width - .5F,
                                       static_cast<float>(y) / width - .5F, 0, 0, 0, 1);
        }
    }
    for (std::uint32_t y = 0; y < width; ++y)
    {
        for (std::uint32_t x = 0; x < width; ++x)
        {
            const auto first = y * (width + 1) + x;
            mesh.indices.insert(mesh.indices.end(), {first, first + 1, first + width + 2, first,
                                                     first + width + 2, first + width + 1});
        }
    }
    auto lod = std::make_unique<schema::MeshLodDataT>();
    lod->screen_height = .2F;
    lod->indices = {0, 8, 80, 0, 80, 72};
    lod->submeshes.emplace_back(0, 6, 0);
    // Two independently bounded half-grids, plus one lower-detail cluster.
    mesh.meshlets = std::make_unique<schema::MeshletDataT>();
    mesh.meshlets->indices = mesh.indices;
    for (std::uint32_t half = 0; half < 2; ++half)
    {
        mesh.meshlets->meshlets.emplace_back(half * 192, 192,
                                             schema::Vec3{0, half == 0 ? -.25 : .25, 0}, .57F,
                                             schema::Vec3{0, 0, 1}, 0);
    }
    mesh.meshlets->submeshes.emplace_back(0, 2);
    lod->meshlets = std::make_unique<schema::MeshletDataT>();
    lod->meshlets->indices = lod->indices;
    lod->meshlets->meshlets.emplace_back(0, 6, schema::Vec3{}, .72F, schema::Vec3{0, 0, 1}, 0);
    lod->meshlets->submeshes.emplace_back(0, 1);
    mesh.lods.push_back(std::move(lod));
    return mesh;
}

schema::LightProbeDataT probes(const char* path, bool left)
{
    schema::LightProbeDataT data;
    data.path = path;
    data.bounds_min = {left ? -3.0 : 1.0, -2, -1};
    data.bounds_max = {left ? -1.0 : 3.0, 2, 1};
    data.grid_x = data.grid_y = data.grid_z = 2;
    data.baked_transform = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    data.valid.assign(8, 1);
    data.coefficients.resize(8 * 9);
    for (std::size_t probe = 0; probe < 8; ++probe)
    {
        data.coefficients[probe * 9] =
            left ? schema::Vec3{.5, .125, .0625} : schema::Vec3{.0625, .125, .5};
    }
    data.source_fingerprint = "render-submission-probes";
    return data;
}

bool set_culling(Camera camera, bool enabled)
{
    auto settings = test_support::read_settings(camera);
    settings.frustum_culling_enabled = enabled;
    return camera.set_settings(settings);
}

bool set_instancing(const std::vector<MeshRenderer>& meshes, bool enabled)
{
    for (auto mesh : meshes)
    {
        if (!mesh.set_static_instancing_enabled(enabled))
        {
            return false;
        }
    }
    return true;
}

bool set_lod(const std::vector<MeshRenderer>& meshes, bool enabled)
{
    for (auto mesh : meshes)
    {
        if (!mesh.set_static_lod_enabled(enabled))
        {
            return false;
        }
    }
    return true;
}

bool frames(WindowSystem& window, RenderSystem& renderer, int count)
{
    int submitted = 0;
    for (int attempt = 0; attempt < 120 && submitted < count; ++attempt)
    {
        if (!window.tick(1.0 / 60.0) || window.exit_requested())
        {
            return false;
        }
        if (!window.can_render())
        {
            SDL_Delay(5);
            continue;
        }
        if (!test_support::render_frame(renderer))
        {
            return false;
        }
        ++submitted;
    }
    return submitted == count;
}

bool capture(WindowSystem& window, RenderSystem& renderer, SceneCaptureImage& image,
             RenderViewStats& stats, bool exhaust_instances = false)
{
    if (!frames(window, renderer, 2) || !renderer.request_hdr_capture())
    {
        return false;
    }
    if (exhaust_instances)
    {
        // Reserve the actual upstream transient pool before the scene draw. No test-only
        // production visibility or fake availability hooks are needed.
        constexpr std::uint16_t stride = 7 * 4 * sizeof(float);
        std::uint64_t reserved = 0;
        while (const auto available = bgfx::getAvailInstanceDataBuffer(4096, stride))
        {
            bgfx::InstanceDataBuffer reservation;
            bgfx::allocInstanceDataBuffer(&reservation, available, stride);
            reserved += available;
            if (!expect(reserved * stride <= bgfx::getCaps()->limits.maxTransientVbSize,
                        "transient reservations remain within the device's finite buffer budget"))
            {
                return false;
            }
        }
        if (!expect(reserved > 0, "instance pool has capacity before explicit exhaustion"))
        {
            return false;
        }
        if (!expect(bgfx::getAvailInstanceDataBuffer(2, stride) < 2,
                    "the real transient instance pool is exhausted"))
        {
            return false;
        }
    }
    for (int attempt = 0;
         attempt < 64 && renderer.hdr_capture_status() == SceneCaptureStatus::pending; ++attempt)
    {
        if (!frames(window, renderer, 1))
        {
            return false;
        }
        if (attempt == 0)
        {
            const auto views = renderer.submission_stats();
            const auto found = std::find_if(views.begin(), views.end(), [](const auto& view)
                                            { return view.view_id == render_detail::main_view; });
            if (found == views.end())
            {
                return false;
            }
            stats = *found;
        }
    }
    if (!renderer.take_hdr_capture(image))
    {
        return false;
    }
    return true;
}

bool pixels(const SceneCaptureImage& left, const SceneCaptureImage& right, const char* message)
{
    if (!expect(left.width == right.width && left.height == right.height &&
                    left.rgba.size() == right.rgba.size() && !left.rgba.empty(),
                "capture dimensions are identical"))
    {
        return false;
    }
    float maximum = 0;
    for (std::size_t index = 0; index < left.rgba.size(); ++index)
    {
        if (!std::isfinite(left.rgba[index]) || !std::isfinite(right.rgba[index]))
        {
            return expect(false, "capture must contain finite values");
        }
        maximum = std::max(maximum, std::abs(left.rgba[index] - right.rgba[index]));
    }
    std::printf("render submission comparison: %s, max linear HDR difference %.9g\n", message,
                maximum);
    // Integer-aligned binary-exact geometry isolates submission changes, not raster placement.
    return expect(maximum == 0, message);
}

int benchmark(int argc, char** argv)
{
    std::filesystem::path output;
    std::uint32_t objects = 4096, settle_frames = 30, measured_frames = 120;
    for (int index = 2; index < argc; index += 2)
    {
        if (index + 1 == argc)
        {
            std::fprintf(stderr, "benchmark failed at line %d (SDL: %s)\n", __LINE__,
                         SDL_GetError());
            return 1;
        }
        const std::string_view option = argv[index], value = argv[index + 1];
        if (option == "--output")
        {
            output = PathUtils::from_utf8(value);
        }
        else
        {
            auto* destination = option == "--objects"         ? &objects
                                : option == "--settle-frames" ? &settle_frames
                                : option == "--frames"        ? &measured_frames
                                                              : nullptr;
            const auto parsed =
                destination
                    ? std::from_chars(value.data(), value.data() + value.size(), *destination)
                    : std::from_chars_result{};
            if (!destination || parsed.ec != std::errc{} ||
                parsed.ptr != value.data() + value.size())
            {
                std::fprintf(stderr, "benchmark failed at line %d (SDL: %s)\n", __LINE__,
                             SDL_GetError());
                return 1;
            }
        }
    }
    if (output.empty() || objects == 0 || objects > 16384 || measured_frames == 0 ||
        measured_frames > 10000 || settle_frames > 10000)
    {
        std::fprintf(stderr, "benchmark failed at line %d (SDL: %s)\n", __LINE__, SDL_GetError());
        return 1;
    }
    std::error_code error;
    if (!std::filesystem::create_directories(output, error) || error)
    {
        std::fprintf(stderr, "benchmark requires a new output directory\n");
        std::fprintf(stderr, "benchmark failed at line %d (SDL: %s)\n", __LINE__, SDL_GetError());
        return 1;
    }
    using Clock = std::chrono::steady_clock;
    const auto elapsed = [](Clock::time_point begin)
    { return std::chrono::duration<double, std::milli>(Clock::now() - begin).count(); };
    const auto begin = Clock::now();
    EngineConfig config;
    config.window_title = "Rendering submission benchmark";
    config.width = 1280;
    config.height = 720;
    config.target_frame_rate = -1;
    test_support::ResourceServices services;
    WorldSystem world(&services.loader);
    WindowSystem window;
    RenderSystem renderer(window, &world);
    test_support::ResourceFixture fixture;
    if (!services.initialize(config) || !services.prepare_builtins() || !world.initialize(config) ||
        !window.initialize(config) || !renderer.initialize(config) || !renderer.set_vsync(false))
    {
        std::fprintf(stderr, "benchmark failed at line %d (SDL: %s)\n", __LINE__, SDL_GetError());
        return 1;
    }
    const double initialize_ms = elapsed(begin);
    const auto load_begin = Clock::now();
    auto geometry = grid();
    geometry.path += "0";
    if (!fixture.add(geometry) ||
        !services.publish_and_load(fixture.catalog, {{"content", fixture.root}}))
    {
        std::fputs("Cannot prepare benchmark mesh\n", stderr);
        return 1;
    }
    const auto side = static_cast<std::uint32_t>(std::ceil(std::sqrt(objects)));
    const double load_ms = elapsed(load_begin);
    const auto scene_begin = Clock::now();
    auto* scene = world.create_scene("Submission benchmark");
    if (!scene)
    {
        std::fprintf(stderr, "benchmark failed at line %d (SDL: %s)\n", __LINE__, SDL_GetError());
        return 1;
    }
    auto camera_node = scene->create_node("Camera");
    auto camera = camera_node.add_component<Camera>();
    auto post = camera_node.add_component<PostProcessComponent>();
    auto environment = scene->create_node("Environment").add_component<EnvironmentComponent>();
    auto light = scene->create_node("Directional").add_component<Light>();
    CameraSettings lens;
    lens.projection = schema::Projection::orthographic;
    lens.orthographic_size = static_cast<float>(side) * 1.1F;
    lens.frustum_culling_enabled = true;
    lens.far_clip = 250;
    TransformSettings camera_pose;
    camera_pose.position = {0, 0, 80};
    auto lighting = test_support::read_settings(environment);
    lighting.ambient_intensity = .3F;
    auto light_settings = test_support::read_settings(light);
    light_settings.cast_shadows = false;
    light_settings.shadow_cascades = 4;
    light_settings.shadow_resolution = 1024;
    auto post_settings = test_support::read_settings(post);
    post_settings.ao_enabled = post_settings.ssr_enabled = false;
    if (!camera || !post || !environment || !light || !camera.set_settings(lens) ||
        !camera_node.get_component<Transform>().set_settings(camera_pose) ||
        !scene->set_active_camera(camera) || !environment.set_settings(lighting) ||
        !light.set_settings(light_settings) || !post.set_settings(post_settings))
    {
        std::fprintf(stderr, "benchmark failed at line %d (SDL: %s)\n", __LINE__, SDL_GetError());
        return 1;
    }
    for (std::uint32_t index = 0; index < objects; ++index)
    {
        auto node = scene->create_node("Mesh " + std::to_string(index));
        auto mesh = node.add_component<MeshRenderer>();
        TransformSettings pose;
        pose.position = {static_cast<double>(index % side) - (side - 1) * .5,
                         static_cast<double>(index / side) - (side - 1) * .5, 0};
        pose.scale = {.8, .8, .8};
        if (!mesh || !mesh.set_mesh(geometry.path) || !mesh.set_static_instancing_enabled(true) ||
            !mesh.set_static_lod_enabled(false) || !mesh.set_material_count(1) ||
            !node.get_component<Transform>().set_settings(pose))
        {
            std::fprintf(stderr, "benchmark failed at line %d (SDL: %s)\n", __LINE__,
                         SDL_GetError());
            return 1;
        }
        auto material = mesh.material();
        if (!material.set_type("builtin://materials/default-pbr") ||
            !material.set_v4("baseColor", {.2, .6, .9, 1.0}) ||
            !material.set_float("metallic", 0) || !material.set_float("roughness", .6F) ||
            !material.set_blend_mode(schema::BlendMode::opaque))
        {
            std::fputs("Cannot configure benchmark material\n", stderr);
            return 1;
        }
    }
    const double scene_ms = elapsed(scene_begin);
    const auto prepare_begin = Clock::now();
    if (!test_support::prepare_render(renderer))
    {
        std::fprintf(stderr, "benchmark failed at line %d (SDL: %s)\n", __LINE__, SDL_GetError());
        return 1;
    }
    const double prepare_ms = elapsed(prepare_begin);
    if (!frames(window, renderer, static_cast<int>(settle_frames)))
    {
        std::fprintf(stderr, "benchmark failed at line %d (SDL: %s)\n", __LINE__, SDL_GetError());
        return 1;
    }
    struct Sample
    {
        double prepare_ms{}, render_ms{}, gpu_ms{-1}, submit_ms{-1};
        std::uint32_t gpu_frame{}, draws{}, compute{}, blits{}, transient_bytes{};
        std::uint16_t textures{}, vertices{}, indices{}, targets{};
        std::int64_t texture_bytes{}, target_bytes{};
        RenderViewStats scene{};
        std::vector<RenderViewStats> views;
    };
    std::vector<Sample> samples(measured_frames);
    [[maybe_unused]] constexpr char sample_begin[] = "Render sample {\"complete\":false}";
    ENGINE_PROFILE_MESSAGE(sample_begin, sizeof(sample_begin) - 1);
    for (std::uint32_t frame = 0; frame < measured_frames; ++frame)
    {
        if (!window.tick(1.0 / 60.0) || !window.can_render())
        {
            std::fprintf(stderr, "benchmark failed at line %d (SDL: %s)\n", __LINE__,
                         SDL_GetError());
            return 1;
        }
        auto& sample = samples[frame];
        const auto prepare_start = Clock::now();
        if (!test_support::prepare_render(renderer))
        {
            std::fprintf(stderr, "benchmark failed at line %d (SDL: %s)\n", __LINE__,
                         SDL_GetError());
            return 1;
        }
        sample.prepare_ms = elapsed(prepare_start);
        const auto render_start = Clock::now();
        if (!(renderer.render() == engine::RenderSubmitStatus::submitted))
        {
            std::fprintf(stderr, "benchmark failed at line %d (SDL: %s)\n", __LINE__,
                         SDL_GetError());
            return 1;
        }
        sample.render_ms = elapsed(render_start);
        const auto views = renderer.submission_stats();
        sample.views.assign(views.begin(), views.end());
        for (const auto& view : views)
        {
            if (view.view_id == render_detail::main_view)
            {
                sample.scene = view;
            }
        }
        const auto& stats = *bgfx::getStats();
        sample.gpu_frame = stats.gpuFrameNum;
        if (stats.gpuTimerFreq > 0 && stats.gpuTimeBegin > 0 &&
            stats.gpuTimeEnd >= stats.gpuTimeBegin)
        {
            sample.gpu_ms = (stats.gpuTimeEnd - stats.gpuTimeBegin) * 1000.0 / stats.gpuTimerFreq;
        }
        if (stats.cpuTimerFreq > 0 && stats.cpuTimeEnd >= stats.cpuTimeBegin)
        {
            sample.submit_ms =
                (stats.cpuTimeEnd - stats.cpuTimeBegin) * 1000.0 / stats.cpuTimerFreq;
        }
        sample.draws = stats.numDraw;
        sample.compute = stats.numCompute;
        sample.blits = stats.numBlit;
        sample.textures = stats.numTextures;
        sample.vertices = stats.numVertexBuffers;
        sample.indices = stats.numIndexBuffers;
        sample.targets = stats.numFrameBuffers;
        sample.texture_bytes = stats.textureMemoryUsed;
        sample.target_bytes = stats.rtMemoryUsed;
        sample.transient_bytes = stats.transientVbUsed;
    }
    [[maybe_unused]] constexpr char sample_end[] = "Render sample {\"complete\":true}";
    ENGINE_PROFILE_MESSAGE(sample_end, sizeof(sample_end) - 1);
    SceneCaptureImage image;
    RenderViewStats image_stats;
    if (!capture(window, renderer, image, image_stats))
    {
        std::fprintf(stderr, "benchmark failed at line %d (SDL: %s)\n", __LINE__, SDL_GetError());
        return 1;
    }
    std::ofstream pixels_file(output / "image.rgba32f", std::ios::binary);
    pixels_file.write(reinterpret_cast<const char*>(image.rgba.data()),
                      static_cast<std::streamsize>(image.rgba.size() * sizeof(float)));
    std::ofstream metadata(output / "metadata.json");
    metadata << "{\"scenario\":\"shared\",\"objects\":" << objects
             << ",\"settle_frames\":" << settle_frames << ",\"frames\":" << measured_frames
             << ",\"width\":" << image.width << ",\"height\":" << image.height
             << ",\"vsync\":false,\"msaa\":1,\"backend\":\""
             << bgfx::getRendererName(bgfx::getRendererType())
             << "\",\"vendor\":" << bgfx::getCaps()->vendorId
             << ",\"device\":" << bgfx::getCaps()->deviceId
             << ",\"initialize_ms\":" << initialize_ms << ",\"load_ms\":" << load_ms
             << ",\"scene_ms\":" << scene_ms << ",\"prepare_ms\":" << prepare_ms << "}\n";
    std::ofstream csv(output / "frames.csv");
    csv << "frame,prepare_ms,render_ms,gpu_frame,gpu_ms,backend_submit_ms,draws,compute,blits,"
           "visible,scene_draws,batches,instances,triangles,textures,vertex_buffers,index_buffers,"
           "framebuffers,texture_bytes,target_bytes,transient_bytes\n";
    for (std::size_t index = 0; index < samples.size(); ++index)
    {
        const auto& s = samples[index];
        csv << index << ',' << s.prepare_ms << ',' << s.render_ms << ',' << s.gpu_frame << ','
            << s.gpu_ms << ',' << s.submit_ms << ',' << s.draws << ',' << s.compute << ','
            << s.blits << ',' << s.scene.visible_meshes << ',' << s.scene.draw_calls << ','
            << s.scene.direct_instance_batches << ',' << s.scene.direct_instances << ','
            << s.scene.direct_triangles << ',' << s.textures << ',' << s.vertices << ','
            << s.indices << ',' << s.targets << ',' << s.texture_bytes << ',' << s.target_bytes
            << ',' << s.transient_bytes << '\n';
    }
    std::ofstream view_csv(output / "views.csv");
    view_csv << "frame,view,candidates,culled,visible,draws,batches,instances,triangles\n";
    for (std::size_t index = 0; index < samples.size(); ++index)
    {
        for (const auto& view : samples[index].views)
        {
            view_csv << index << ',' << view.view_id << ',' << view.candidate_meshes << ','
                     << view.culled_meshes << ',' << view.visible_meshes << ',' << view.draw_calls
                     << ',' << view.direct_instance_batches << ',' << view.direct_instances << ','
                     << view.direct_triangles << '\n';
        }
    }
    std::vector<double> times;
    for (const auto& sample : samples)
    {
        times.push_back(sample.render_ms);
    }
    std::sort(times.begin(), times.end());
    const auto percentile = [&](double p)
    { return times[static_cast<std::size_t>(p * (times.size() - 1))]; };
    std::ofstream summary(output / "summary.json");
    summary << "{\"render_mean_ms\":"
            << std::accumulate(times.begin(), times.end(), 0.0) / times.size()
            << ",\"render_p50_ms\":" << percentile(.5) << ",\"render_p95_ms\":" << percentile(.95)
            << ",\"render_p99_ms\":" << percentile(.99) << ",\"render_max_ms\":" << times.back()
            << "}\n";
    const bool written =
        pixels_file.good() && metadata.good() && csv.good() && view_csv.good() && summary.good();
    std::printf(
        "benchmark shared: %u objects, render P50 %.3f / P95 %.3f / P99 %.3f ms, draws %u\n",
        objects, percentile(.5), percentile(.95), percentile(.99), samples.back().draws);
    renderer.shutdown();
    world.destroy_scene();
    return written ? 0 : 1;
}

int test()
{
    EngineConfig config;
    config.window_title = "Upstream rendering submission regression";
    config.width = config.height = 256;
    test_support::ResourceServices services;
    auto& resources = services.core;
    engine::WorldSystem world(&services.loader);
    WindowSystem window;
    RenderConfig render_config;
    render_config.maximum_meshlet_candidates = 16;
    RenderSystem renderer(window, &world, nullptr, nullptr, nullptr, render_config);
    test_support::ResourceFixture fixture;
    const auto geometry = grid();
    const auto left_probes = probes("lighting/submission-left", true);
    const auto right_probes = probes("lighting/submission-right", false);
    if (!(services.initialize(config) && services.prepare_builtins()) ||
        !world.initialize(config) || !window.initialize(config) || !renderer.initialize(config) ||
        !fixture.add(geometry) || !fixture.add(left_probes) || !fixture.add(right_probes) ||
        !services.publish_and_load(fixture.catalog, {{"content", fixture.root}}))
    {
        return 1;
    }
    if (bgfx::getCaps()->limits.maxInstanceData < 7)
    {
        std::fprintf(stderr, "render submission: device has fewer than 7 instance-data slots\n");
        return 77;
    }
    auto* scene = world.create_scene("Submission paths");
    auto camera_node = scene ? scene->create_node("Camera") : Node{};
    auto camera = camera_node ? camera_node.add_component<Camera>() : Camera{};
    auto post =
        camera_node ? camera_node.add_component<PostProcessComponent>() : PostProcessComponent{};
    auto environment_node = scene ? scene->create_node("Environment") : Node{};
    auto environment = environment_node ? environment_node.add_component<EnvironmentComponent>()
                                        : EnvironmentComponent{};
    auto light_node = scene ? scene->create_node("Directional light") : Node{};
    auto light = light_node ? light_node.add_component<Light>() : Light{};
    CameraSettings lens;
    lens.projection = schema::Projection::orthographic;
    lens.orthographic_size = 4;
    lens.frustum_culling_enabled = false;
    TransformSettings camera_pose;
    camera_pose.position = {0, 0, 6};
    auto lighting = test_support::read_settings(environment);
    lighting.ambient_intensity = .3F;
    if (!camera || !post || !environment || !light || !camera.set_settings(lens) ||
        !camera_node.get_component<Transform>().set_settings(camera_pose) ||
        !scene->set_active_camera(camera) || !environment.set_settings(lighting))
    {
        return 1;
    }
    std::vector<MeshRenderer> visible;
    std::vector<MeshRenderer> meshes;
    for (int index = 0; index < 14; ++index)
    {
        auto node = scene->create_node("Mesh " + std::to_string(index));
        auto mesh = node ? node.add_component<MeshRenderer>() : MeshRenderer{};
        TransformSettings pose;
        pose.position = index < 6 ? Vec3{(index % 3 - 1) * 2.0, index / 3 == 0 ? -1.0 : 1.0, 0}
                                  : Vec3{20.0 + index, 0, 0};
        if (!mesh || !mesh.set_mesh(geometry.path) || !mesh.set_static_instancing_enabled(false) ||
            !mesh.set_static_lod_enabled(false) ||
            !node.get_component<Transform>().set_settings(pose) ||
            !mesh.material().set_type("builtin://materials/default-pbr") ||
            !mesh.material().set_v4("baseColor", {.2, .6, .9, 1}) ||
            !mesh.material().set_float("metallic", 0) || !mesh.material().set_float("roughness", 1))
        {
            return 1;
        }
        meshes.push_back(mesh);
        if (index < 6)
        {
            visible.push_back(mesh);
        }
    }
    SceneCaptureImage baseline, culled, instanced, changed_baseline, changed_instanced,
        lod_baseline, lod_instanced, exhausted, probe_baseline, probe_instanced, instance_opt_out,
        lod_opt_out;
    RenderViewStats all_stats, cull_stats, instance_stats, changed_stats, changed_instance_stats,
        lod_stats, lod_instance_stats, exhausted_stats, probe_stats, probe_instance_stats,
        instance_opt_out_stats, lod_opt_out_stats;
    if (!capture(window, renderer, baseline, all_stats) || !set_culling(camera, true) ||
        !capture(window, renderer, culled, cull_stats) ||
        !pixels(baseline, culled, "frustum culling preserves every pixel") ||
        !expect(all_stats.candidate_meshes == 14 && all_stats.culled_meshes == 0 &&
                    cull_stats.culled_meshes == 8 && cull_stats.visible_meshes == 6 &&
                    cull_stats.draw_calls < all_stats.draw_calls,
                "culling removes only the eight truly offscreen submissions") ||
        !set_instancing(meshes, true) || !capture(window, renderer, instanced, instance_stats) ||
        !pixels(culled, instanced, "shared opaque instances preserve every pixel") ||
        !expect(instance_stats.direct_instance_batches > 0 &&
                    instance_stats.draw_calls < cull_stats.draw_calls &&
                    instance_stats.direct_instances == 6 &&
                    instance_stats.direct_triangles == cull_stats.direct_triangles,
                "instances reduce draw calls without losing meshes or triangles"))
    {
        return 1;
    }
    const auto has_surface_view = [&]()
    {
        const auto views = renderer.submission_stats();
        return std::any_of(views.begin(), views.end(),
                           [](const auto& view) { return view.view_id == 111; });
    };
    auto screen_effects = test_support::read_settings(post);
    SceneCaptureImage ssr_image, ssr_restored;
    RenderViewStats ssr_stats;
    if (!expect(!has_surface_view(), "SSR off does not submit a material surface pass"))
    {
        return 1;
    }
    screen_effects.ssr_enabled = true;
    if (!post.set_settings(screen_effects) || !capture(window, renderer, ssr_image, ssr_stats) ||
        !expect(has_surface_view(), "SSR on prepares and submits its material surface pass"))
    {
        return 1;
    }
    screen_effects.ssr_enabled = false;
    if (!post.set_settings(screen_effects) || !capture(window, renderer, ssr_restored, ssr_stats) ||
        !expect(!has_surface_view(), "disabling SSR removes its material surface pass") ||
        !pixels(instanced, ssr_restored, "disabling SSR restores the exact HDR image"))
    {
        return 1;
    }
    constexpr auto meshlet_caps = BGFX_CAPS_COMPUTE | BGFX_CAPS_DRAW_INDIRECT;
    const bool meshlet_supported = (bgfx::getCaps()->supported & meshlet_caps) == meshlet_caps &&
                                   bgfx::getCaps()->limits.maxInstanceData >= 7 &&
                                   bgfx::getCaps()->limits.maxComputeBindings >= 5;
    const auto meshlet_pixels = [&](const SceneCaptureImage& reference, const char* message,
                                    bool verify_merged_singletons = false)
    {
        const auto ordinary_compute = bgfx::getStats()->numCompute;
        auto settings = test_support::read_settings(environment);
        settings.meshlet_culling_enabled = true;
        SceneCaptureImage image;
        RenderViewStats stats;
        if (!environment.set_settings(settings) ||
            !expect(renderer.render() == RenderSubmitStatus::submitted,
                    "enabling before GPU preparation keeps the ordinary frame drawable") ||
            !capture(window, renderer, image, stats) || !pixels(reference, image, message) ||
            !expect(!meshlet_supported || stats.indirect_batches > 0,
                    "compatible geometry uses actual GPU indirect submissions") ||
            !expect(!verify_merged_singletons || !meshlet_supported ||
                        (stats.indirect_commands == stats.indirect_batches &&
                         stats.meshlet_candidate_pairs == stats.indirect_batches * 2),
                    "consecutive singleton clusters share one command without losing candidates") ||
            !expect(!meshlet_supported || bgfx::getStats()->numCompute <= ordinary_compute + 1,
                    "all prepared batches share one culling dispatch"))
        {
            return false;
        }
        settings.meshlet_culling_enabled = false;
        return environment.set_settings(settings);
    };
    if (!meshlet_pixels(instanced, "Meshlet indirect instances preserve every pixel"))
    {
        return 1;
    }
    if (!set_instancing(meshes, false) ||
        !meshlet_pixels(culled, "merged singleton commands preserve every pixel", true) ||
        !set_instancing(meshes, true))
    {
        return 1;
    }
    if ((bgfx::getCaps()->supported & BGFX_CAPS_COMPUTE) != 0 &&
        bgfx::getCaps()->limits.maxComputeBindings >= 4)
    {
        SceneCaptureImage colored, repeated, restored;
        RenderViewStats color_stats;
        if (!renderer.set_debug_feature(RenderFeature::meshlet_colors, true) ||
            !capture(window, renderer, colored, color_stats) ||
            !expect(renderer.feature_state(RenderFeature::meshlet_colors).effective &&
                        !test_support::read_settings(environment).meshlet_culling_enabled &&
                        color_stats.indirect_batches == 0,
                    "cluster colors work independently of scene culling") ||
            !expect(std::inner_product(colored.rgba.begin(), colored.rgba.end(),
                                       instanced.rgba.begin(), std::size_t{0}, std::plus<>{},
                                       [](float a, float b)
                                       { return std::abs(a - b) > .05F; }) > 100,
                    "cluster colors replace actual surface pixels") ||
            !capture(window, renderer, repeated, color_stats) ||
            !pixels(colored, repeated, "cluster colors are stable across frames") ||
            !renderer.set_debug_feature(RenderFeature::meshlet_colors, false) ||
            !capture(window, renderer, restored, color_stats) ||
            !pixels(instanced, restored, "closing cluster colors restores the exact PBR image"))
        {
            return 1;
        }
    }
    lighting.meshlet_culling_enabled = true;
    SceneCaptureImage budget_image;
    RenderViewStats budget_stats;
    if (!set_culling(camera, false) || !environment.set_settings(lighting) ||
        !capture(window, renderer, budget_image, budget_stats) ||
        !pixels(baseline, budget_image, "Meshlet capacity fallback preserves the whole batch") ||
        !expect(!meshlet_supported ||
                    (budget_stats.meshlet_budget_fallbacks == 14 &&
                     budget_stats.indirect_batches == 0 && budget_stats.direct_instances == 14),
                "candidate overflow falls back without dropping or splitting instances"))
    {
        return 1;
    }
    lighting.meshlet_culling_enabled = false;
    if (!environment.set_settings(lighting) || !set_culling(camera, true))
    {
        return 1;
    }
    if (!visible[0].set_static_instancing_enabled(false) ||
        !capture(window, renderer, instance_opt_out, instance_opt_out_stats) ||
        !pixels(culled, instance_opt_out,
                "one mesh can opt out without changing other instances") ||
        !expect(instance_opt_out_stats.draw_calls == 2 &&
                    instance_opt_out_stats.direct_instance_batches == 1 &&
                    instance_opt_out_stats.direct_instances == 6 &&
                    visible[1].static_instancing_enabled(),
                "one ordinary draw and one five-instance batch respect local mesh attributes") ||
        !visible[0].set_static_instancing_enabled(true))
    {
        return 1;
    }
    if (!capture(window, renderer, exhausted, exhausted_stats, true) ||
        !pixels(culled, exhausted,
                "exhausted transient instances fall back without losing pixels") ||
        !expect(exhausted_stats.direct_instance_batches == 0 &&
                    exhausted_stats.draw_calls == cull_stats.draw_calls &&
                    exhausted_stats.direct_instances == cull_stats.direct_instances &&
                    exhausted_stats.direct_triangles == cull_stats.direct_triangles,
                "no transient capacity safely selects ordinary draws for every mesh"))
    {
        return 1;
    }
    auto mirrored = test_support::read_settings(visible[1].node().get_component<Transform>());
    mirrored.scale = {-1, 1, 1};
    if (!visible[0].material().set_v4("baseColor", {.9, .2, .1, 1}) ||
        !visible[1].node().get_component<Transform>().set_settings(mirrored) ||
        !set_instancing(meshes, false) ||
        !capture(window, renderer, changed_baseline, changed_stats) ||
        !set_instancing(meshes, true) ||
        !capture(window, renderer, changed_instanced, changed_instance_stats) ||
        !pixels(changed_baseline, changed_instanced,
                "material overrides and mirrored culling stay isolated") ||
        !expect(changed_instance_stats.draw_calls >= 3 &&
                    changed_instance_stats.direct_instance_batches > 0,
                "material and mirrored winding differences split incompatible batches"))
    {
        return 1;
    }
    auto large = test_support::read_settings(visible[0].node().get_component<Transform>());
    large.scale = {2.5, 2.5, 1};
    if (!visible[0].material().set_v4("baseColor", {.2, .6, .9, 1}) ||
        !visible[0].node().get_component<Transform>().set_settings(large) ||
        !set_instancing(meshes, false) || !set_lod(meshes, true) ||
        !capture(window, renderer, lod_baseline, lod_stats) || !set_instancing(meshes, true) ||
        !capture(window, renderer, lod_instanced, lod_instance_stats) ||
        !pixels(lod_baseline, lod_instanced,
                "different LOD topology cannot be merged into one batch") ||
        !expect(lod_instance_stats.direct_triangles == geometry.indices.size() / 3 + 5 * 2 &&
                    lod_stats.direct_triangles == lod_instance_stats.direct_triangles &&
                    lod_instance_stats.direct_triangles < cull_stats.direct_triangles &&
                    lod_instance_stats.draw_calls >= 3 &&
                    lod_instance_stats.direct_instance_batches > 0,
                "one large LOD0 and five small LOD1 instances submit the exact expected triangle "
                "count"))
    {
        return 1;
    }
    if (!meshlet_pixels(lod_instanced, "Meshlet LOD and mirrored materials preserve every pixel"))
    {
        return 1;
    }
    if (!visible[2].set_static_lod_enabled(false) ||
        !capture(window, renderer, lod_opt_out, lod_opt_out_stats) ||
        !expect(lod_opt_out_stats.direct_triangles == 2 * geometry.indices.size() / 3 + 4 * 2 &&
                    visible[3].static_lod_enabled(),
                "disabling LOD on one small mesh preserves its original topology only") ||
        !visible[2].set_static_lod_enabled(true))
    {
        return 1;
    }
    large.scale = {1, 1, 1};
    mirrored.scale = {1, 1, 1};
    if (!visible[0].node().get_component<Transform>().set_settings(large) ||
        !visible[1].node().get_component<Transform>().set_settings(mirrored) ||
        !set_lod(meshes, false))
    {
        return 1;
    }
    for (const auto* resource : {&left_probes, &right_probes})
    {
        auto node = scene->create_node(resource->path);
        auto volume = node.add_component<LightProbeVolumeComponent>();
        auto settings = test_support::read_settings(volume);
        if (!volume ||
            !resources.catalog().identities().path_to_guid(resource->path, settings.probes_guid) ||
            !volume.set_settings(settings))
        {
            return 1;
        }
    }
    if (!set_instancing(meshes, false) || !capture(window, renderer, probe_baseline, probe_stats) ||
        !set_instancing(meshes, true) ||
        !capture(window, renderer, probe_instanced, probe_instance_stats) ||
        !pixels(probe_baseline, probe_instanced,
                "incompatible probe lighting bindings cannot share a batch") ||
        !expect(
            probe_instanced.rgba != culled.rgba &&
                probe_instance_stats.direct_instance_batches == 3 &&
                probe_instance_stats.draw_calls == 3 && probe_instance_stats.direct_instances == 6,
            "left probes, right probes and unbound meshes form three distinct lighting batches"))
    {
        return 1;
    }
    auto receiver_node = scene->create_node("Shadow receiver");
    auto receiver = receiver_node.add_component<MeshRenderer>();
    TransformSettings receiver_pose;
    receiver_pose.position = {0, 0, -2};
    receiver_pose.scale = {12, 8, 1};
    TransformSettings sun_pose;
    sun_pose.rotation = {0, -35, 0};
    auto shadow_settings = test_support::read_settings(light);
    shadow_settings.cast_shadows = true;
    shadow_settings.shadow_resolution = 512;
    shadow_settings.shadow_cascades = 4;
    SceneCaptureImage shadow_ordinary, shadow_instanced;
    RenderViewStats shadow_stats;
    const auto sample_shadow_draws = [&](std::uint32_t& peak)
    {
        peak = 0;
        // Include the changed frame after the upstream submission/render queue catches up.
        for (unsigned index = 0; index < 4; ++index)
        {
            if (!frames(window, renderer, 1))
            {
                return false;
            }
            peak = std::max(peak, bgfx::getStats()->numDraw);
        }
        return true;
    };
    std::uint32_t ordinary_draws = 0, instanced_draws = 0;
    if (!receiver || !receiver.set_mesh(geometry.path) ||
        !receiver_node.get_component<Transform>().set_settings(receiver_pose) ||
        !light_node.get_component<Transform>().set_settings(sun_pose) ||
        !light.set_settings(shadow_settings) ||
        !visible[1].material().set_float("alphaCutoff", .5F) ||
        !visible[1].material().set_v4("baseColor", {.2, .6, .9, .25}) ||
        !set_instancing(meshes, false) || !sample_shadow_draws(ordinary_draws) ||
        !capture(window, renderer, shadow_ordinary, shadow_stats))
    {
        return 1;
    }
    if (!set_instancing(meshes, true) || !sample_shadow_draws(instanced_draws) ||
        !capture(window, renderer, shadow_instanced, shadow_stats) ||
        !pixels(
            shadow_ordinary, shadow_instanced,
            "shadow batching preserves alpha clipping, mirrored culling and off-camera casters") ||
        !expect(instanced_draws < ordinary_draws, "shadow instancing reduces actual backend draws"))
    {
        return 1;
    }
    const auto cached_draws = bgfx::getStats()->numDraw;
    const auto refreshed_shadow = [&](const char* message)
    {
        std::uint32_t peak = 0;
        return sample_shadow_draws(peak) && expect(peak > cached_draws, message);
    };
    SceneCaptureImage ordinary_cached, ordinary_changed;
    auto caster_pose = test_support::read_settings(visible[0].node().get_component<Transform>());
    const auto saved_caster_pose = caster_pose;
    caster_pose.position.mutate_x(caster_pose.position.x() + .4);
    if (!expect(cached_draws < instanced_draws, "unchanged shadows omit backend atlas draws") ||
        !capture(window, renderer, ordinary_cached, shadow_stats) ||
        !pixels(shadow_instanced, ordinary_cached,
                "cached ordinary shadows preserve every pixel") ||
        !visible[0].node().get_component<Transform>().set_settings(caster_pose) ||
        !refreshed_shadow("moving a caster redraws the ordinary atlas") ||
        !capture(window, renderer, ordinary_changed, shadow_stats) ||
        !expect(ordinary_changed.rgba != ordinary_cached.rgba,
                "moving a caster produces a changed image") ||
        !visible[0].node().get_component<Transform>().set_settings(saved_caster_pose) ||
        !visible[1].material().set_float("alphaCutoff", 0) ||
        !refreshed_shadow("changing alpha clipping redraws the ordinary atlas") ||
        !visible[1].material().set_float("alphaCutoff", .5F))
    {
        return 1;
    }
    auto changed_sun = sun_pose;
    changed_sun.rotation = {0, -40, 0};
    auto changed_camera = camera_pose;
    changed_camera.position.mutate_x(.1);
    if (!light_node.get_component<Transform>().set_settings(changed_sun) ||
        !refreshed_shadow("moving a light redraws the ordinary atlas") ||
        !light_node.get_component<Transform>().set_settings(sun_pose) ||
        !camera_node.get_component<Transform>().set_settings(changed_camera) ||
        !refreshed_shadow("moving the camera redraws the ordinary atlas") ||
        !camera_node.get_component<Transform>().set_settings(camera_pose))
    {
        return 1;
    }
    visible[0].set_cast_shadows(false);
    if (!refreshed_shadow("changing a caster's shadow flag redraws the ordinary atlas"))
    {
        return 1;
    }
    visible[0].set_cast_shadows(true);
    auto deformed = geometry.vertices;
    std::vector<Vec4> deformed_tangents(deformed.size(), {1, 0, 0, 1});
    deformed.front().mutate_z(.1F);
    if (!visible[0].set_deformation(deformed, deformed_tangents) ||
        !refreshed_shadow("deformed casters use ordinary per-frame shadow submission") ||
        !capture(window, renderer, ordinary_changed, shadow_stats) ||
        !expect(bgfx::getStats()->numDraw > cached_draws,
                "unchanged dynamic buffers do not enable static atlas reuse"))
    {
        return 1;
    }
    visible[0].clear_deformation();
    if (!capture(window, renderer, ordinary_cached, shadow_stats) ||
        !pixels(shadow_instanced, ordinary_cached,
                "restoring all shadow inputs reconstructs the original atlas"))
    {
        return 1;
    }
    shadow_settings.shadow_method = schema::ShadowMethod::virtual_shadow_map;
    shadow_settings.virtual_shadow = std::make_unique<schema::VsmSettingsT>();
    shadow_settings.virtual_shadow->virtual_resolution = 4096;
    VsmStatus vsm;
    const auto settle_vsm = [&]()
    {
        for (unsigned attempt = 0; attempt < 160; ++attempt)
        {
            if (!frames(window, renderer, 1) || !renderer.vsm_status(vsm))
            {
                return false;
            }
            if (vsm.state == VsmState::failed || vsm.state == VsmState::unsupported)
            {
                break;
            }
            if (vsm.state == VsmState::ready && vsm.demanded_pages && !vsm.redrawn_pages)
            {
                return true;
            }
        }
        std::fprintf(stderr, "VSM state=%u demand=%u ready=%u fallback=%u error=%s\n",
                     unsigned(vsm.state), vsm.demanded_pages, vsm.ready_pages, vsm.fallback_pages,
                     vsm.error.c_str());
        return false;
    };
    SceneCaptureImage virtual_image, cached_image, moved_image, shadow_restored;
    if (!light.set_settings(shadow_settings) ||
        !expect(settle_vsm(), "GPU receiver feedback warms virtual pages without blocking") ||
        !capture(window, renderer, virtual_image, shadow_stats) ||
        !capture(window, renderer, cached_image, shadow_stats) ||
        !pixels(virtual_image, cached_image, "static cached virtual shadows keep identical pixels"))
    {
        return 1;
    }
    const auto original_generation = vsm.generation;
    if (!camera.set_enabled(false) || !frames(window, renderer, 2) || !renderer.vsm_status(vsm) ||
        !expect(vsm.state == VsmState::disabled && !vsm.resident_pages && !vsm.resident_bytes,
                "a scene without a drawable camera retires virtual shadow state") ||
        !camera.set_enabled(true) ||
        !expect(settle_vsm() && vsm.generation > original_generation,
                "restoring the camera warms a new VSM generation") ||
        !capture(window, renderer, cached_image, shadow_stats) ||
        !pixels(virtual_image, cached_image, "restoring the camera reconstructs virtual shadows"))
    {
        return 1;
    }
    const auto old_cache = vsm.cache_revision;
    auto moved_pose = test_support::read_settings(visible[0].node().get_component<Transform>());
    const auto original_pose = moved_pose;
    moved_pose.position.mutate_x(moved_pose.position.x() + .4);
    if (!visible[0].node().get_component<Transform>().set_settings(moved_pose) ||
        !expect(settle_vsm() && vsm.cache_revision > old_cache,
                "moving a caster refreshes cached virtual shadows") ||
        !capture(window, renderer, moved_image, shadow_stats) ||
        !expect(moved_image.rgba != cached_image.rgba, "moving caster changes rendered pixels") ||
        !visible[0].node().get_component<Transform>().set_settings(original_pose))
    {
        return 1;
    }
    shadow_settings.shadow_method = schema::ShadowMethod::shadow_map;
    if (!light.set_settings(shadow_settings) ||
        !capture(window, renderer, shadow_restored, shadow_stats) ||
        !pixels(shadow_instanced, shadow_restored, "disabling VSM restores ordinary shadows") ||
        !renderer.vsm_status(vsm) ||
        !expect(vsm.state == VsmState::disabled && !vsm.resident_pages && !vsm.resident_bytes,
                "disabling VSM releases its resident state"))
    {
        return 1;
    }
    // Both reflection shadow ranges remain live while main-view meshlet commands run.
    for (unsigned index = 0; index < 2; ++index)
    {
        auto node = scene->create_node("Reflection and meshlet composition");
        auto mesh = node.add_component<MeshRenderer>();
        auto reflection = node.add_component<PlanarReflectionComponent>();
        TransformSettings pose;
        pose.position = {0, -2.0 - index, 0};
        pose.scale = {6, .025, 6};
        if (!mesh || !reflection || !mesh.set_mesh("builtin://meshes/cube") ||
            !node.get_component<Transform>().set_settings(pose) ||
            !mesh.material().set_type("builtin://materials/default-pbr") ||
            !mesh.material().set_float("metallic", 1) ||
            !mesh.material().set_float("roughness", .1F))
        {
            return 1;
        }
    }
    SceneCaptureImage reflected_reference;
    if (!capture(window, renderer, reflected_reference, shadow_stats))
    {
        return 1;
    }
    camera_pose.position = {0, 3, 8};
    camera_pose.rotation = {-20, 0, 0};
    if (!camera_node.get_component<Transform>().set_settings(camera_pose) ||
        !capture(window, renderer, reflected_reference, shadow_stats))
    {
        return 1;
    }
    const auto reflected_views = renderer.submission_stats();
    if (!expect(std::any_of(reflected_views.begin(), reflected_views.end(),
                            [](const auto& value) { return value.view_id == 33; }) &&
                    std::any_of(reflected_views.begin(), reflected_views.end(),
                                [](const auto& value) { return value.view_id == 72; }),
                "both nondegenerate planar captures submit actual geometry") ||
        !meshlet_pixels(reflected_reference,
                        "Meshlet culling with two planar captures and CSM preserves every pixel"))
    {
        return 1;
    }
    std::printf("render submission: draws %u -> %u -> %u; LOD triangles %llu -> %llu\n",
                all_stats.draw_calls, cull_stats.draw_calls, instance_stats.draw_calls,
                static_cast<unsigned long long>(cull_stats.direct_triangles),
                static_cast<unsigned long long>(lod_instance_stats.direct_triangles));
    renderer.shutdown();
    world.destroy_scene();
    return 0;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc > 1)
    {
        return std::string_view(argv[1]) == "--benchmark" ? benchmark(argc, argv) : 1;
    }
    return test();
}
