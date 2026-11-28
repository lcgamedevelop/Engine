#pragma once

#include "runtime/render/frame_graph_plan.hpp"

#include <array>
#include <bgfx/bgfx.h>
#include <span>

#include <cstdint>

namespace engine::render_detail
{

struct GpuGeometry;
struct GpuVertex;

// GPU 计算输出与普通网格使用相同顶点布局，供全部绘制通道复用。
class SkinningPass final
{
public:
    SkinningPass() = default;
    ~SkinningPass() noexcept;
    SkinningPass(const SkinningPass&) = delete;
    SkinningPass& operator=(const SkinningPass&) = delete;
    [[nodiscard]] bool prepare();
    [[nodiscard]] bool deform(const GpuGeometry& geometry,
                              std::span<const std::array<float, 16>> matrices,
                              bgfx::DynamicVertexBufferHandle& palette,
                              bgfx::DynamicVertexBufferHandle& output);
    [[nodiscard]] bool upload_vertices(std::span<const GpuVertex> vertices,
                                       bgfx::DynamicVertexBufferHandle& output);
    [[nodiscard]] std::uint32_t resource_count() const noexcept;
    void release() noexcept;

private:
    [[nodiscard]] bool dispatch(const GpuGeometry& geometry,
                                bgfx::DynamicVertexBufferHandle palette,
                                bgfx::DynamicVertexBufferHandle output, std::uint32_t joint_count);
    FrameGraphPlan compute_plan_, upload_plan_;
    bgfx::ShaderHandle shader_ = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle program_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle parameters_ = BGFX_INVALID_HANDLE;
};

} // namespace engine::render_detail
