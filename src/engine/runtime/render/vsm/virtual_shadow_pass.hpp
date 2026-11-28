#pragma once

#include "runtime/render/frame_graph_plan.hpp"
#include "runtime/render/gpu_readback.hpp"
#include "runtime/render/vsm/vsm_types.hpp"

#include <deque>
#include <span>

namespace engine
{
struct RenderConfig;
namespace render_detail
{
class ShadowPass;
struct RenderFrameData;
struct ShadowRecord;
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
                                       std::span<const std::uint64_t> depth_inputs,
                                       std::uint64_t submission_id);
    void accept_feedback(std::uint64_t receiver_revision) noexcept;
    [[nodiscard]] bool draw_pages(const RenderFrameData& frame, const VsmPageBatch& batch,
                                  ShadowPass& shadow, VirtualShadowCache& cache);
    [[nodiscard]] float submit_local_depth_ranges(bgfx::TextureHandle atlas,
                                                  std::span<const ShadowRecord> records,
                                                  std::uint64_t depth_revision);
    [[nodiscard]] bool binding(VsmGpuBinding& output) const;
    void reset_scene() noexcept;
    void release_gpu() noexcept;
    [[nodiscard]] std::uint32_t resource_count() const noexcept;

private:
    void release_depth_ranges() noexcept;

    struct Readback
    {
        VsmView view;
        std::vector<std::uint32_t> bits;
        GpuReadback completion;
        std::uint64_t submission_id = 0, receiver_revision = 0;
        bool pending = false;
    };
    FrameGraphPlan feedback_plan_, page_plan_, local_range_plan_;
    std::size_t planned_pages_ = 0, planned_local_faces_ = 0;
    bool planned_ranges_ = false;
    std::array<Readback, 3> readbacks_;
    std::deque<VsmPageDemand> completed_;
    VsmView feedback_view_;
    std::vector<std::uint64_t> feedback_inputs_;
    std::uint64_t receiver_revision_ = 0, accepted_receiver_revision_ = 0;
    std::uint64_t generation_ = 0, bytes_ = 0;
    std::uint32_t resolution_ = 0, physical_pages_ = 0, word_count_ = 0;
    bgfx::TextureHandle atlas_ = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle depth_ranges_ = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle fallback_ = BGFX_INVALID_HANDLE;
    bgfx::FrameBufferHandle framebuffer_ = BGFX_INVALID_HANDLE;
    bgfx::DynamicIndexBufferHandle demands_ = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle clear_ = BGFX_INVALID_HANDLE, feedback_ = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle reduce_depth_ranges_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle sampler_ = BGFX_INVALID_HANDLE, depth_sampler_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle inverse_ = BGFX_INVALID_HANDLE, dimensions_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle view_uniform_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle range_tile_origins_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle range_output_ = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle local_range_atlas_ = BGFX_INVALID_HANDLE;
    std::uint64_t local_range_revision_ = 0;
    std::uint16_t local_range_first_row_ = 0, local_range_stride_ = 0, local_range_faces_ = 0;
    std::vector<std::array<float, 4>> local_range_tiles_;
};
} // namespace render_detail
} // namespace engine
