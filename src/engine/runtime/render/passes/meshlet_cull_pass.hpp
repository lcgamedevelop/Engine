#pragma once

#include "runtime/render/render_features.hpp"

#include <bgfx/bgfx.h>

#include <array>
#include <chrono>
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
    struct Readback
    {
        std::array<std::uint32_t, 4> counters{};
        MeshletGpuStats stats;
        std::uint32_t ready_frame = 0;
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
    bgfx::ProgramHandle program_ = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle clear_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle parameters_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle planes_ = BGFX_INVALID_HANDLE;
    bgfx::DynamicVertexBufferHandle inputs_ = BGFX_INVALID_HANDLE;
    bgfx::DynamicVertexBufferHandle instances_ = BGFX_INVALID_HANDLE;
    bgfx::DynamicVertexBufferHandle descriptors_ = BGFX_INVALID_HANDLE;
    bgfx::IndirectBufferHandle commands_ = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle counters_ = BGFX_INVALID_HANDLE;
};

[[nodiscard]] bool meshlet_supported() noexcept;
} // namespace engine::render_detail
