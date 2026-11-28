#pragma once

#include "runtime/render/gpu_readback.hpp"
#include "runtime/render/passes/meshlet_graph.hpp"
#include "runtime/render/render_features.hpp"

#include <bgfx/bgfx.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <map>
#include <optional>
#include <vector>

namespace engine
{
struct RenderConfig;
}

namespace engine::render_detail
{
struct RenderFrameData;
struct SceneDrawView;
struct ViewDrawList;

// Owned by RenderSystem: pending readback destinations outlive scene GPU resources.
class MeshletCullPass final
{
public:
    MeshletCullPass() = default;
    ~MeshletCullPass() noexcept;
    MeshletCullPass(const MeshletCullPass&) = delete;
    MeshletCullPass& operator=(const MeshletCullPass&) = delete;
    [[nodiscard]] bool prepare(const RenderConfig& config);
    [[nodiscard]] bool dispatch(const RenderFrameData& frame, const SceneDrawView& view,
                                ViewDrawList& draws, bool sample);
    void poll(std::uint32_t frame_number);
    [[nodiscard]] MeshletGpuStats statistics() const noexcept;
    void reset_scene() noexcept;
    void release_gpu() noexcept;
    [[nodiscard]] std::uint32_t resource_count() const noexcept;

private:
    [[nodiscard]] bool prepare_index_compaction();
    void release_index_compaction() noexcept;
    FrameGraphPlan submission_plan_;
    std::optional<MeshletGraphShape> submission_shape_;

    struct Readback
    {
        std::array<std::uint32_t, 4> counters{};
        GpuReadback completion;
        MeshletGpuStats stats;
        std::uint64_t generation = 0;
        bool pending = false;
    };
    std::array<Readback, 3> readbacks_;
    MeshletGpuStats stats_;
    std::chrono::steady_clock::time_point sampled_at_{};
    std::uint64_t generation_ = 0, world_epoch_ = 0;
    std::array<std::uint64_t, 2> observer_{};
    std::vector<std::uint64_t> resources_;
    bool sampling_ = false;
    std::uint32_t capacity_ = 0;
    std::uint32_t input_capacity_ = 0, instance_capacity_ = 0, command_capacity_ = 0;
    std::uint32_t descriptor_capacity_ = 0;
    std::vector<std::uint64_t> descriptor_layout_;
    std::vector<std::array<float, 4>> uploaded_inputs_;
    std::array<std::array<float, 4>, 6> result_planes_{};
    std::uint32_t result_flags_ = 0;
    bool result_valid_ = false;
    std::uint64_t result_revision_ = 0;
    bgfx::ProgramHandle program_ = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle clear_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle parameters_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle planes_ = BGFX_INVALID_HANDLE;
    bgfx::DynamicVertexBufferHandle inputs_ = BGFX_INVALID_HANDLE;
    bgfx::DynamicVertexBufferHandle instances_ = BGFX_INVALID_HANDLE;
    bgfx::DynamicVertexBufferHandle descriptors_ = BGFX_INVALID_HANDLE;
    bgfx::IndirectBufferHandle commands_ = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle counters_ = BGFX_INVALID_HANDLE;
    bool index_compaction_attempted_ = false;
    std::uint32_t index_region_capacity_ = 0, index_source_count_ = 0;
    std::map<std::array<std::uint64_t, 5>, std::uint32_t> index_sources_;
    std::vector<std::array<std::uint32_t, 4>> uploaded_index_jobs_;
    bgfx::DynamicIndexBufferHandle index_arena_ = BGFX_INVALID_HANDLE;
    bgfx::DynamicVertexBufferHandle index_compaction_jobs_ = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle index_compaction_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle index_compaction_parameters_ = BGFX_INVALID_HANDLE;
};

[[nodiscard]] bool meshlet_supported() noexcept;
[[nodiscard]] bool meshlet_index_compaction_supported() noexcept;
} // namespace engine::render_detail
