#pragma once

#include "runtime/performance/function_profile.hpp"

#include <bgfx/bgfx.h>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace engine::render_detail
{

struct SceneDrawView;
struct RenderFrameData;
struct ViewDrawList;

// 拥有屏幕空间效果共用的单采样深度与材质表面附件。
class SurfacePass final
{
public:
    SurfacePass() = default;
    ~SurfacePass() noexcept;
    SurfacePass(const SurfacePass&) = delete;
    SurfacePass& operator=(const SurfacePass&) = delete;

    [[nodiscard]] bool prepare(std::uint16_t width, std::uint16_t height, bool material_surface);
    [[nodiscard]] bool can_combine_depth(const RenderFrameData& frame,
                                         const ViewDrawList& list) const;
    void begin(const float* view, const float* projection, bool combined_depth);
    [[nodiscard]] std::uint64_t identify_depth(const SceneDrawView& draw,
                                               const RenderFrameData& frame,
                                               const ViewDrawList& list, bool include_surface);
    [[nodiscard]] bool has_surface(std::uint64_t revision) const noexcept
    {
        return revision != 0 && revision == surface_revision_;
    }
    void publish_surface(std::uint64_t revision) noexcept
    {
        surface_revision_ = revision;
    }
    void reserve_cached_instances() const;
    [[nodiscard]] std::span<const std::uint64_t> depth_inputs() const noexcept
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        return depth_inputs_;
    }
    [[nodiscard]] bgfx::TextureHandle depth() const noexcept;
    [[nodiscard]] bgfx::TextureHandle normal_roughness() const noexcept;
    [[nodiscard]] bgfx::TextureHandle specular() const noexcept;
    [[nodiscard]] std::uint32_t resource_count() const noexcept;
    void release() noexcept;

private:
    std::array<bgfx::TextureHandle, 3> targets_{
        {BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE}};
    bgfx::FrameBufferHandle framebuffer_ = BGFX_INVALID_HANDLE;
    std::vector<std::uint64_t> depth_inputs_, candidate_inputs_;
    std::uint64_t depth_revision_ = 0, surface_revision_ = 0;
    std::uint32_t transient_instances_ = 0;
    std::uint16_t width_ = 0;
    std::uint16_t height_ = 0;
    bool material_surface_ = false;
};

} // namespace engine::render_detail
