#pragma once

#include "runtime/scene/scene_types.hpp"

#include <array>
#include <cstdint>

namespace engine
{

enum class RenderFeature : std::uint8_t
{
    frustum_culling,
    static_instancing,
    static_lod,
    meshlet_culling,
    wireframe,
    debug_draw,
    mesh_bounds,
    camera_frusta,
    meshlet_bounds,
    meshlet_colors,
    meshlet_statistics,
    stats,
    count
};

enum class RenderFeatureReason : std::uint8_t
{
    none,
    device_unavailable,
    device_unsupported,
    no_scene,
    no_applicable_draws,
    resources_unavailable,
    capacity_limited
};

struct RenderFeatureState
{
    bool requested = false;
    bool supported = false;
    bool effective = false;
    RenderFeatureReason reason = RenderFeatureReason::none;
};

enum class MeshletSampleState : std::uint8_t
{
    disabled,
    unsupported,
    pending,
    ready,
    failed
};

struct MeshletGpuStats
{
    MeshletSampleState state = MeshletSampleState::disabled;
    std::uint64_t world_epoch = 0;
    std::array<std::uint64_t, 2> observer{};
    std::uint32_t frame_number = 0;
    std::uint64_t candidate_pairs = 0, candidate_triangles = 0;
    std::uint64_t visible_pairs = 0, nonempty_commands = 0, visible_triangles = 0;
};

// CPU submissions and explicitly asynchronous GPU diagnostic samples remain separate.
struct RenderViewStats
{
    std::uint32_t frame_number = 0;
    std::uint16_t view_id = 0;
    std::uint32_t candidate_meshes = 0;
    std::uint32_t culled_meshes = 0;
    std::uint32_t visible_meshes = 0;
    std::uint32_t draw_calls = 0;
    std::uint32_t direct_instance_batches = 0;
    std::uint32_t direct_instances = 0;
    std::uint64_t direct_triangles = 0;
    std::uint32_t indirect_batches = 0, indirect_commands = 0;
    std::uint32_t meshlet_instanced_batches = 0;
    std::uint64_t meshlet_candidate_pairs = 0, meshlet_candidate_triangles = 0;
    std::uint32_t meshlet_missing_data = 0, meshlet_incompatible_draws = 0;
    std::uint32_t meshlet_budget_fallbacks = 0;
    MeshletGpuStats meshlet_gpu;
};

struct DebugLine
{
    Vec3 start{};
    Vec3 end{};
    std::uint32_t rgba = 0xffffffff;
    bool depth_test = true;
};

} // namespace engine
