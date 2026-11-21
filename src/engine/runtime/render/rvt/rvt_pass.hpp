#pragma once

#include "runtime/performance/function_profile.hpp"
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
    void set_diagnostics(std::uint32_t flags) noexcept
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        diagnostics_ = flags;
    }
    [[nodiscard]] const schema::RvtSettingsT& settings() const noexcept
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        return settings_;
    }

private:
    struct Source
    {
        std::uint64_t instance = 0, revision = 0, deformation = 0;
        std::array<double, 16> model{};
        std::vector<MaterialBinding> materials;
        Bounds3 bounds;
    };
    schema::RvtSettingsT settings_;
    std::uint64_t device_ = 0, source_revision_ = 0;
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
