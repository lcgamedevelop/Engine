#pragma once

#include "runtime/performance/function_profile.hpp"
#include "runtime/render/frame_graph_plan.hpp"
#include "runtime/render/render_submission.hpp"
#include "runtime/render/rvt/virtual_texture_cache.hpp"
#include <bgfx/bgfx.h>
#include <map>

namespace engine::render_detail
{
// Owns GPU content only. Allocation/demand policy remains in VirtualTextureCache.
class RvtPass final
{
public:
    ~RvtPass() noexcept;
    [[nodiscard]] bool configure(const schema::RvtSettingsT& settings, std::uint64_t device);
    [[nodiscard]] bool prepare_pages(VirtualTextureCache& cache, const RenderFrameData& sources);
    void invalidate_sources(VirtualTextureCache& cache, const RenderFrameData& sources);
    void bind(bool receiver, const Vec3& origin) const;
    static void bind_disabled();
    void release_gpu() noexcept;
    [[nodiscard]] bool available() const noexcept
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        return bgfx::isValid(target_);
    }
    [[nodiscard]] std::uint64_t resident_bytes() const noexcept;
    [[nodiscard]] const std::string& reason() const noexcept
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        return reason_;
    }
    void set_diagnostics(std::uint32_t flags) noexcept;
    // Zero until the atlas and page table have been published together.
    [[nodiscard]] std::uint64_t surface_revision() const noexcept
    {
        return surface_revision_;
    }
    [[nodiscard]] const schema::RvtSettingsT& settings() const noexcept
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        return settings_;
    }

private:
    [[nodiscard]] bool prepare_plan(std::uint16_t pages, std::uint8_t levels);
    [[nodiscard]] bool draw_page(const RvtPage& page, const Bounds3& area, bgfx::ViewId view,
                                 std::span<const RenderFrameData::Mesh* const> writers);
    void upload_table_level(const VirtualTextureCache& cache, std::uint8_t mip);
    FrameGraphPlan plan_;
    std::uint16_t planned_pages_ = frame_invalid_id;
    std::uint8_t planned_levels_ = 0;
    bool initialize_atlas_ = true, planned_clear_ = false;
    struct Source
    {
        std::uint64_t instance = 0, revision = 0, deformation = 0;
        std::array<double, 16> model{};
        std::vector<MaterialBinding> materials;
        std::vector<std::uint64_t> texture_generations;
        Bounds3 bounds;
    };
    schema::RvtSettingsT settings_;
    std::uint64_t device_ = 0, source_revision_ = 0;
    std::uint64_t published_cache_revision_ = 0, surface_revision_ = 0;
    std::uint32_t atlas_size_ = 0, slots_per_row_ = 0, diagnostics_ = 0;
    std::uint8_t minimum_mip_ = 0;
    std::map<std::uint64_t, Source> sources_;
    bgfx::FrameBufferHandle target_ = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle depth_ = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle atlas_ = BGFX_INVALID_HANDLE, table_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle atlas_sampler_ = BGFX_INVALID_HANDLE, table_sampler_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle bounds_ = BGFX_INVALID_HANDLE, layout_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle parameters_ = BGFX_INVALID_HANDLE, height_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle normal_ = BGFX_INVALID_HANDLE, surface_ = BGFX_INVALID_HANDLE;
    std::string reason_;
};
} // namespace engine::render_detail
