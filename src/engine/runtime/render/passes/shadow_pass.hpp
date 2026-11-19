#pragma once

#include "runtime/render/passes/shadow_frame.hpp"

#include <bgfx/bgfx.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace engine
{
struct RenderConfig;
} // namespace engine

namespace engine::render_detail
{

struct RenderFrameData;
struct MaterialBinding;
struct VsmPageDraw;
struct VsmPageTarget;

inline constexpr std::uint64_t shadow_atlas_flags =
    BGFX_TEXTURE_RT | BGFX_SAMPLER_UVW_CLAMP | BGFX_SAMPLER_POINT;

struct ShadowAtlasView
{
    std::uint16_t index = 0, count = 1;
};

class ShadowPass final
{
public:
    ShadowPass() = default;
    ~ShadowPass() noexcept;
    ShadowPass(const ShadowPass&) = delete;
    ShadowPass& operator=(const ShadowPass&) = delete;
    ShadowPass(ShadowPass&&) = delete;
    ShadowPass& operator=(ShadowPass&&) = delete;

    [[nodiscard]] bool prepare(std::uint16_t size);
    [[nodiscard]] bool draw(const RenderFrameData& frame_data, const RenderSettings& settings,
                            ShadowFrame& frame, bgfx::TextureHandle fallback,
                            const RenderConfig& config,
                            std::span<const LocalShadowLight> local_lights,
                            std::span<ClusterLight> lights, const float* camera_world,
                            bgfx::ViewId base_view, ShadowAtlasView atlas_view,
                            const std::span<const std::uint64_t>* caster_ids = nullptr);
    [[nodiscard]] bool draw_pages(const RenderFrameData& frame, std::span<const VsmPageDraw> pages,
                                  const VsmPageTarget& target);
    [[nodiscard]] bgfx::TextureHandle texture() const noexcept;
    [[nodiscard]] std::uint64_t local_depth_revision() const noexcept;
    [[nodiscard]] std::uint32_t resource_count() const noexcept;
    void release() noexcept;

private:
    struct LocalProjection
    {
        Vec3 position;
        std::array<float, 3> direction;
        std::array<float, 3> up;

        bool operator==(const LocalProjection&) const = default;
    };

    struct CachedPage
    {
        std::uint16_t x, y, size;
        bgfx::ViewId producer_view;
        std::array<float, 16> view;
        std::array<float, 16> projection;
        std::array<float, 4> caster_clip_xy;
        Vec3 caster_origin;
        std::optional<LocalProjection> local_projection;
    };

    struct SolidRange
    {
        std::size_t source = 0;
        std::uint32_t first = 0, count = 0, original_draws = 0;
    };

    struct SolidBatch
    {
        bgfx::VertexBufferHandle positions = BGFX_INVALID_HANDLE;
        bgfx::IndexBufferHandle indices = BGFX_INVALID_HANDLE;
        std::uint64_t state = 0;
        std::vector<SolidRange> ranges;
    };

    [[nodiscard]] bool bind_material(const MaterialBinding& material, bgfx::TextureHandle fallback);
    void prepare_solid_batches(const RenderFrameData& frame);
    void release_solid_batches() noexcept;

    bgfx::TextureHandle depth_ = BGFX_INVALID_HANDLE;
    bgfx::FrameBufferHandle framebuffer_ = BGFX_INVALID_HANDLE;
    bgfx::ShaderHandle vertex_ = BGFX_INVALID_HANDLE;
    bgfx::ShaderHandle fragment_ = BGFX_INVALID_HANDLE;
    bgfx::ShaderHandle solid_fragment_ = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle program_ = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle solid_program_ = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle solid_batch_program_ = BGFX_INVALID_HANDLE;
    bgfx::ShaderHandle instanced_vertex_ = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle instanced_program_ = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle instanced_solid_program_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle alpha_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle alpha_sampler_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle uv_transform_ = BGFX_INVALID_HANDLE;
    std::uint16_t resolution_ = 0;
    std::uint64_t local_depth_revision_ = 0;
    std::vector<bgfx::ViewId> attached_views_;
    std::uint32_t last_skipped_lights_ = 0;
    std::vector<CachedPage> cached_pages_;
    std::vector<std::uint64_t> caster_signature_;
    std::vector<std::uint64_t> candidate_signature_;
    std::vector<SolidBatch> solid_batches_;
    std::vector<std::uint8_t> solid_sources_;
    std::size_t solid_batch_bytes_ = 0;
    bool solid_inputs_seen_ = false;
};

} // namespace engine::render_detail
