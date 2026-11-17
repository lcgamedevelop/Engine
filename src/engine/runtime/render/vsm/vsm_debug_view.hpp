#pragma once
#include "runtime/render/vsm.hpp"
#include "runtime/resource/resource_reference.hpp"
#include <bgfx/bgfx.h>
namespace engine
{
class FontSystem;
namespace render_detail
{
class FrameOutput;
class VirtualShadowPass;
class ClusteredLightingPass;
class VsmDebugView final
{
public:
    explicit VsmDebugView(FontSystem& fonts) noexcept : fonts_(fonts)
    {
    }
    ~VsmDebugView() noexcept;
    [[nodiscard]] bool prepare_font(ResourceLoader& resources);
    [[nodiscard]] bool draw(const VsmStatus& status, const VirtualShadowPass* pass,
                            const ClusteredLightingPass* lighting, const VsmDebugOptions& options,
                            const FrameOutput& output);
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
