#include "runtime/render/render_system.hpp"

#include "runtime/render/passes/debug_draw_pass.hpp"
#include "runtime/render/passes/meshlet_cull_pass.hpp"
#include "runtime/render/render_constants.hpp"
#include "runtime/render/render_math.hpp"
#include "runtime/render/scene_gpu_resources.hpp"
#include "runtime/scene/ecs_storage.hpp"
#include "runtime/scene/mesh_renderer.hpp"
#include "runtime/scene/scene.hpp"
#include "runtime/world/world_system.hpp"

#include <algorithm>
#include <cmath>

namespace engine
{
namespace
{
constexpr auto feature_count = static_cast<std::size_t>(RenderFeature::count);
constexpr std::size_t maximum_debug_lines = 65536;

bool finite(const Vec3& value)
{
    return std::isfinite(value.x()) && std::isfinite(value.y()) && std::isfinite(value.z());
}

bool supported(RenderFeature feature)
{
    const auto* caps = bgfx::getCaps();
    if (caps == nullptr)
    {
        return false;
    }
    if (feature == RenderFeature::static_instancing)
    {
        return caps->limits.maxInstanceData >= 7;
    }
    if (feature == RenderFeature::meshlet_culling || feature == RenderFeature::meshlet_statistics)
    {
        return render_detail::meshlet_supported();
    }
    if (feature == RenderFeature::wireframe)
    {
        return render_detail::DebugDrawPass::supports_wireframe();
    }
    if (feature == RenderFeature::meshlet_colors)
    {
        return render_detail::DebugDrawPass::supports_meshlet_colors();
    }
    return true;
}
} // namespace

bool RenderSystem::set_debug_feature(RenderFeature feature, bool enabled)
{
    const auto index = static_cast<std::size_t>(feature);
    if (index < static_cast<std::size_t>(RenderFeature::wireframe) || index >= feature_count ||
        (enabled &&
         (feature == RenderFeature::wireframe || feature == RenderFeature::meshlet_colors) &&
         (!renderer_initialized_ || !supported(feature))))
    {
        return false;
    }
    debug_requests_[index] = enabled;
    if (feature == RenderFeature::stats && renderer_initialized_)
    {
        bgfx::setDebug(enabled ? BGFX_DEBUG_STATS : BGFX_DEBUG_NONE);
    }
    if (feature == RenderFeature::debug_draw && !enabled)
    {
        clear_debug_lines();
    }
    return true;
}

RenderFeatureState RenderSystem::feature_state(RenderFeature feature) const noexcept
{
    RenderFeatureState result;
    const auto index = static_cast<std::size_t>(feature);
    if (index >= feature_count)
    {
        result.reason = RenderFeatureReason::device_unsupported;
        return result;
    }
    if (feature == RenderFeature::frustum_culling)
    {
        const auto camera = world_ && world_->scene() ? world_->scene()->active_camera() : Camera{};
        CameraSettings settings;
        result.requested = camera.get_settings(settings) && settings.frustum_culling_enabled;
    }
    else if (feature == RenderFeature::static_instancing || feature == RenderFeature::static_lod)
    {
        if (world_ && world_->scene())
        {
            auto query = flecs::world(world_->native_world())
                             .query_builder<const ecs_detail::ComponentStorage<MeshRenderer::Data>,
                                            const ecs_detail::NodeRuntime>()
                             .with<ecs_detail::Published>()
                             .build();
            query.each(
                [&](const auto& stored, const ecs_detail::NodeRuntime& node)
                {
                    if (node.active && node.visible_in_hierarchy && stored.state.flags.enabled &&
                        stored.state.flags.visible)
                    {
                        result.requested =
                            result.requested || (feature == RenderFeature::static_instancing
                                                     ? stored.data.static_instancing_enabled
                                                     : stored.data.static_lod_enabled);
                    }
                });
        }
    }
    else if (feature == RenderFeature::meshlet_culling)
    {
        result.requested = settings().meshlet_culling_enabled;
    }
    else
    {
        result.requested = debug_requests_[index];
    }
    if (!renderer_initialized_)
    {
        result.reason = RenderFeatureReason::device_unavailable;
        return result;
    }
    result.supported = supported(feature);
    if (!result.supported)
    {
        result.reason = RenderFeatureReason::device_unsupported;
        return result;
    }
    result.effective = feature_applied_[index];
    if (!result.requested && !result.effective)
    {
        return result;
    }
    if (feature != RenderFeature::stats && (world_ == nullptr || world_->scene() == nullptr))
    {
        result.effective = false;
        result.reason = RenderFeatureReason::no_scene;
    }
    else if (!result.effective)
    {
        result.reason = RenderFeatureReason::no_applicable_draws;
    }
    if (feature == RenderFeature::meshlet_bounds && result.requested && scene_resources_ &&
        scene_resources_->meshlet_bounds_limited)
    {
        result.reason = RenderFeatureReason::capacity_limited;
    }
    return result;
}

std::vector<RenderViewStats> RenderSystem::submission_stats() const
{
    if (scene_resources_ == nullptr ||
        std::any_of(scene_resources_->view_stats.begin(), scene_resources_->view_stats.end(),
                    [](const auto& row) { return row.frame_number == 0; }))
    {
        // A failed or still-being-assembled frame is not a published statistics snapshot.
        return {};
    }
    return scene_resources_->view_stats;
}

bool RenderSystem::draw_debug_lines(std::span<const DebugLine> lines, std::uint16_t lifetime_frames)
{
    if (!is_initialized() || !renderer_initialized_ || world_ == nullptr ||
        world_->scene() == nullptr ||
        !debug_requests_[static_cast<std::size_t>(RenderFeature::debug_draw)] ||
        lifetime_frames == 0 || lifetime_frames > 600 || lines.empty() ||
        lines.size() > maximum_debug_lines ||
        std::any_of(lines.begin(), lines.end(),
                    [](const auto& line) { return !finite(line.start) || !finite(line.end); }))
    {
        return false;
    }
    std::size_t count = lines.size();
    for (const auto& batch : debug_lines_)
    {
        count += batch.lines.size();
    }
    if (count > maximum_debug_lines)
    {
        return false;
    }
    debug_lines_.push_back({{lines.begin(), lines.end()}, lifetime_frames, world_->scene()->id()});
    return true;
}

void RenderSystem::clear_debug_lines() noexcept
{
    debug_lines_.clear();
}

void RenderSystem::publish_features() noexcept
{
    feature_applied_.fill(false);
    feature_applied_[static_cast<std::size_t>(RenderFeature::stats)] =
        debug_requests_[static_cast<std::size_t>(RenderFeature::stats)];
    if (scene_resources_ == nullptr)
    {
        return;
    }
    for (auto& row : scene_resources_->view_stats)
    {
        row.frame_number = frame_number_;
        if (row.view_id == render_detail::main_view)
        {
            row.meshlet_gpu = meshlet_culling_ ? meshlet_culling_->statistics() : MeshletGpuStats{};
            if (debug_requests_[static_cast<std::size_t>(RenderFeature::meshlet_statistics)] &&
                !supported(RenderFeature::meshlet_statistics))
            {
                row.meshlet_gpu.state = MeshletSampleState::unsupported;
            }
        }
    }
    feature_applied_[static_cast<std::size_t>(RenderFeature::frustum_culling)] =
        scene_resources_->culling_applied;
    feature_applied_[static_cast<std::size_t>(RenderFeature::static_lod)] =
        scene_resources_->lod_applied;
    feature_applied_[static_cast<std::size_t>(RenderFeature::static_instancing)] = std::any_of(
        scene_resources_->view_stats.begin(), scene_resources_->view_stats.end(),
        [](const auto& row)
        { return row.direct_instance_batches > 0 || row.meshlet_instanced_batches > 0; });
    feature_applied_[static_cast<std::size_t>(RenderFeature::meshlet_culling)] =
        std::any_of(scene_resources_->view_stats.begin(), scene_resources_->view_stats.end(),
                    [](const auto& row) { return row.indirect_batches > 0; });
    feature_applied_[static_cast<std::size_t>(RenderFeature::meshlet_statistics)] =
        meshlet_culling_ &&
        debug_requests_[static_cast<std::size_t>(RenderFeature::meshlet_statistics)] &&
        meshlet_culling_->statistics().state == MeshletSampleState::ready;
    const auto& debug = scene_resources_->debug_draw;
    for (const auto feature : {RenderFeature::wireframe, RenderFeature::debug_draw,
                               RenderFeature::mesh_bounds, RenderFeature::camera_frusta,
                               RenderFeature::meshlet_bounds, RenderFeature::meshlet_colors})
    {
        const auto index = static_cast<std::size_t>(feature);
        feature_applied_[index] = debug_requests_[index] && debug.submitted(feature);
    }
    if (debug.submitted(RenderFeature::debug_draw))
    {
        for (auto& batch : debug_lines_)
        {
            if (batch.frames > 0)
            {
                --batch.frames;
            }
        }
    }
    std::erase_if(debug_lines_, [](const auto& batch) { return batch.frames == 0; });
}

bool RenderSystem::draw_debug_overlay(Camera camera, const float* view, const float* projection)
{
    if (!scene_resources_ || !world_ || !world_->scene())
    {
        return true;
    }
    std::vector<DebugLine> lines;
    const auto scene_id = world_->scene()->id();
    std::erase_if(debug_lines_,
                  [scene_id](const auto& batch) { return batch.scene_id != scene_id; });
    for (const auto& batch : debug_lines_)
    {
        lines.insert(lines.end(), batch.lines.begin(), batch.lines.end());
    }
    std::vector<std::size_t> levels;
    levels.reserve(scene_resources_->meshes.size());
    for (const auto& mesh : scene_resources_->meshes)
    {
        const auto selected =
            scene_resources_->selected_lods.find({mesh.instance_id, render_detail::main_view});
        levels.push_back(selected == scene_resources_->selected_lods.end() ? 0 : selected->second);
    }
    std::vector<DebugLine> meshlet_lines;
    scene_resources_->meshlet_bounds_limited = false;
    if (debug_requests_[static_cast<std::size_t>(RenderFeature::meshlet_bounds)])
    {
        const auto& frame = scene_resources_->frame_data;
        for (const auto& item : scene_resources_->draw_lists[2].opaque)
        {
            const auto& mesh = frame.meshes[item.mesh];
            const auto* data = item.lod == 0 ? mesh.data->meshlets.get()
                                             : mesh.data->lods[item.lod - 1]->meshlets.get();
            if (!data || !mesh.instance_eligible)
            {
                continue;
            }
            std::size_t part = 0;
            const auto ranges = item.geometry->submeshes(item.lod);
            if (!ranges.empty())
            {
                const auto found = std::find_if(ranges.begin(), ranges.end(),
                                                [&](const auto& range)
                                                {
                                                    return range.first_index() == item.first &&
                                                           range.index_count() == item.count;
                                                });
                if (found == ranges.end())
                {
                    return false;
                }
                part = static_cast<std::size_t>(found - ranges.begin());
            }
            const auto& range = data->submeshes[part];
            for (std::uint32_t index = range.first_meshlet();
                 index < range.first_meshlet() + range.meshlet_count(); ++index)
            {
                constexpr std::uint32_t segments = 16;
                if (lines.size() + meshlet_lines.size() + segments * 3 > maximum_debug_lines)
                {
                    scene_resources_->meshlet_bounds_limited = true;
                    break;
                }
                const auto& cluster = data->meshlets[index];
                std::uint32_t hash = 2166136261U;
                for (const auto c : mesh.data->path)
                {
                    hash = (hash ^ static_cast<unsigned char>(c)) * 16777619U;
                }
                hash = (hash ^ (index + static_cast<std::uint32_t>(item.lod) * 65537U)) * 16777619U;
                const auto rgba = ((hash & 0x7f7f7f00U) | 0x808080ffU);
                for (std::size_t plane = 0; plane < 3; ++plane)
                {
                    const auto point = [&](std::uint32_t step)
                    {
                        const auto angle = static_cast<float>(step) * 6.28318530718F / segments;
                        std::array<float, 3> local{static_cast<float>(cluster.center().x()),
                                                   static_cast<float>(cluster.center().y()),
                                                   static_cast<float>(cluster.center().z())};
                        local[(plane + 1) % 3] += std::cos(angle) * cluster.radius();
                        local[(plane + 2) % 3] += std::sin(angle) * cluster.radius();
                        const auto world = render_detail::transform_point(mesh.model.data(), local);
                        return Vec3(world[0], world[1], world[2]);
                    };
                    for (std::uint32_t step = 0; step < segments; ++step)
                    {
                        meshlet_lines.push_back({point(step), point(step + 1), rgba, true});
                    }
                }
            }
            if (scene_resources_->meshlet_bounds_limited)
            {
                break;
            }
        }
    }
    return scene_resources_->debug_draw.draw(
        *world_, camera, scene_resources_->meshes,
        scene_resources_->targets().hdr_post_process.scene_framebuffer(),
        static_cast<std::uint16_t>(width_), static_cast<std::uint16_t>(height_), view, projection,
        debug_requests_, lines, levels, meshlet_lines, scene_resources_->frame_data,
        scene_resources_->draw_lists[2]);
}
} // namespace engine
