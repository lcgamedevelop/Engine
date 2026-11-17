#pragma once

#include "runtime/render/vsm/vsm_types.hpp"

#include <deque>

namespace engine
{
struct RenderConfig;
namespace render_detail
{
class ShadowPass;
struct RenderFrameData;
class VirtualShadowPass final
{
public:
    VirtualShadowPass() = default;
    ~VirtualShadowPass() noexcept;
    VirtualShadowPass(const VirtualShadowPass&) = delete;
    VirtualShadowPass& operator=(const VirtualShadowPass&) = delete;
    [[nodiscard]] bool prepare(const schema::VsmSettingsT& settings, const RenderConfig& config,
                               std::uint64_t generation);
    void poll(std::uint32_t frame_number);
    [[nodiscard]] bool take_demand(VsmPageDemand& output);
    [[nodiscard]] bool submit_feedback(const VsmView& view, bgfx::TextureHandle depth,
                                       std::uint64_t submission_id);
    [[nodiscard]] bool draw_pages(const RenderFrameData& frame, const VsmPageBatch& batch,
                                  ShadowPass& shadow);
    [[nodiscard]] bool binding(VsmGpuBinding& output) const;
    void reset_scene() noexcept;
    void release_gpu() noexcept;
    [[nodiscard]] std::uint32_t resource_count() const noexcept;

private:
    struct Readback
    {
        VsmView view;
        std::vector<std::uint32_t> bits;
        std::uint64_t submission_id = 0;
        std::uint32_t ready_frame = 0;
        bool pending = false;
    };
    std::array<Readback, 3> readbacks_;
    std::deque<VsmPageDemand> completed_;
    std::uint64_t generation_ = 0, bytes_ = 0;
    std::uint32_t resolution_ = 0, physical_pages_ = 0, word_count_ = 0;
    bgfx::TextureHandle atlas_ = BGFX_INVALID_HANDLE, depth_ = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle fallback_ = BGFX_INVALID_HANDLE;
    bgfx::FrameBufferHandle framebuffer_ = BGFX_INVALID_HANDLE;
    bgfx::DynamicIndexBufferHandle demands_ = BGFX_INVALID_HANDLE;
    bgfx::DynamicVertexBufferHandle metadata_ = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle clear_ = BGFX_INVALID_HANDLE, feedback_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle sampler_ = BGFX_INVALID_HANDLE, depth_sampler_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle inverse_ = BGFX_INVALID_HANDLE, dimensions_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle layout_ = BGFX_INVALID_HANDLE, shadow_depth_ = BGFX_INVALID_HANDLE;
};
} // namespace render_detail
} // namespace engine
