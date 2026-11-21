#pragma once

#include "runtime/performance/function_profile.hpp"
#include "runtime/render/rvt.hpp"
#include "runtime/resource/resource_reference.hpp"
#include <bgfx/bgfx.h>
namespace engine
{
class FontSystem;
namespace render_detail
{
class FrameOutput;
class RvtPass;
class RvtDebugView final
{
public:
    explicit RvtDebugView(FontSystem& fonts) noexcept : fonts_(fonts)
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
    }
    ~RvtDebugView() noexcept;
    [[nodiscard]] bool prepare_font(ResourceLoader& resources);
    [[nodiscard]] bool draw(const RvtStatus& status, const RvtPass* pass,
                            const RvtDebugOptions& options, const FrameOutput& output);
    void release_gpu() noexcept;

private:
    FontSystem& fonts_;
    ResourceReference font_, shader_;
    std::unique_ptr<ResourceLoad> font_pending_, shader_pending_;
    bgfx::ProgramHandle program_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle options_ = BGFX_INVALID_HANDLE;
};
} // namespace render_detail
} // namespace engine
