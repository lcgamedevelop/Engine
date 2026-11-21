#include "runtime/render/passes/planar_reflection_pass.hpp"
#include "runtime/performance/profiling.hpp"
#include "runtime/render/passes/planar_reflection_math.hpp"
#include "runtime/render/render_color.hpp"
#include "runtime/render/render_sampling.hpp"
#include "runtime/scene/camera_math.hpp"
#include <algorithm>
#include <bx/math.h>
#include <cmath>

namespace engine::render_detail
{
PlanarReflectionPass::~PlanarReflectionPass() noexcept
{
    ENGINE_PROFILE_FUNCTION();
    release();
}

bool PlanarReflectionPass::prepare(std::uint16_t new_width, std::uint16_t new_height,
                                   std::uint8_t samples)
{
    ENGINE_PROFILE_FUNCTION();
    if (new_width == width && new_height == height && samples_ == samples &&
        bgfx::isValid(framebuffer_))
    {
        return true;
    }
    release();
    const auto* caps = bgfx::getCaps();
    if (!caps || !valid_msaa_samples(samples) || new_width == 0 || new_height == 0 ||
        new_width > caps->limits.maxTextureSize || new_height > caps->limits.maxTextureSize ||
        caps->limits.maxFBAttachments < 3)
    {
        return false;
    }
    const auto flags = render_target_flags(samples);
    const auto write_only = flags | BGFX_TEXTURE_RT_WRITE_ONLY;
    const auto sample_capability = render_target_sample_capability(samples);
    color_ = bgfx::createTexture2D(new_width, new_height, false, 1, bgfx::TextureFormat::RGBA16F,
                                   flags | BGFX_SAMPLER_UVW_CLAMP);
    // Only the first color is sampled. Keep the shared PBR output layout at a lower storage cost.
    if ((caps->formats[bgfx::TextureFormat::R8] & sample_capability) != 0)
    {
        specular_ = bgfx::createTexture2D(new_width, new_height, false, 1, bgfx::TextureFormat::R8,
                                          write_only);
    }
    if (!bgfx::isValid(specular_))
    {
        specular_ = bgfx::createTexture2D(new_width, new_height, false, 1,
                                          bgfx::TextureFormat::RGBA16F,
                                          flags | BGFX_SAMPLER_UVW_CLAMP);
    }
    if (samples > 1 && (caps->formats[bgfx::TextureFormat::D32FS8] & sample_capability) != 0)
    {
        depth_ = bgfx::createTexture2D(new_width, new_height, false, 1, bgfx::TextureFormat::D32FS8,
                                       write_only);
    }
    const bool stencil = bgfx::isValid(depth_);
    if (!bgfx::isValid(depth_))
    {
        depth_ = bgfx::createTexture2D(new_width, new_height, false, 1, bgfx::TextureFormat::D32F,
                                       write_only);
    }
    if (bgfx::isValid(color_) && bgfx::isValid(specular_) && bgfx::isValid(depth_))
    {
        const std::array attachments = {color_, specular_, depth_};
        framebuffer_ = bgfx::createFrameBuffer(3, attachments.data(), false);
    }
    if (!bgfx::isValid(framebuffer_))
    {
        release();
        return false;
    }
    if (stencil)
    {
        depth_framebuffer_ = bgfx::createFrameBuffer(1, &depth_, false);
    }
    width = new_width;
    height = new_height;
    samples_ = samples;
    return true;
}

bool PlanarReflectionPass::mirror(const float* plane_world, const float* camera_world,
                                  const float* camera_projection, float clip_bias)
{
    ENGINE_PROFILE_FUNCTION();
    const auto* caps = bgfx::getCaps();
    return caps != nullptr && prepare_planar_reflection(
                                  plane_world, camera_world, camera_projection, clip_bias,
                                  caps->homogeneousDepth, world, view, projection, view_projection);
}

bool PlanarReflectionPass::begin_opaque_depth(bgfx::ViewId view_id,
                                              const PlanarReflectionScissor& scissor)
{
    ENGINE_PROFILE_FUNCTION();
    if (!bgfx::isValid(depth_framebuffer_))
    {
        return false;
    }
    attached_depth_view_ = view_id;
    bgfx::setViewName(view_id, "Planar reflection depth prepass");
    bgfx::setViewFrameBuffer(view_id, depth_framebuffer_);
    bgfx::setViewRect(view_id, 0, 0, width, height);
    bgfx::setViewScissor(view_id, scissor.x, scissor.y, scissor.width, scissor.height);
    bgfx::setViewTransform(view_id, view.data(), projection.data());
    bgfx::setViewClear(view_id, BGFX_CLEAR_DEPTH | BGFX_CLEAR_STENCIL, 0, 1.0F, 0);
    bgfx::touch(view_id);
    return true;
}

void PlanarReflectionPass::begin(bgfx::ViewId view_id, std::uint32_t clear_color,
                                 bool depth_prepared, const PlanarReflectionScissor& scissor)
{
    ENGINE_PROFILE_FUNCTION();
    attached_view_ = view_id;
    // Each view has its own palette slot; subsequent views cannot change this clear.
    const auto palette = static_cast<std::uint8_t>(2 + view_id / 40);
    std::array<float, 4> clear{};
    for (std::size_t index = 0; index < 3; ++index)
    {
        clear[index] = display_srgb_to_linear(
            static_cast<float>((clear_color >> ((3 - index) * 8)) & 255) / 255);
    }
    clear[3] = 1;
    bgfx::setPaletteColor(palette, clear.data());
    bgfx::setViewName(view_id, "Planar reflection capture");
    bgfx::setViewFrameBuffer(view_id, framebuffer_);
    bgfx::setViewRect(view_id, 0, 0, width, height);
    bgfx::setViewScissor(view_id, scissor.x, scissor.y, scissor.width, scissor.height);
    const auto clear_flags = BGFX_CLEAR_COLOR | (depth_prepared ? 0 : BGFX_CLEAR_DEPTH);
    bgfx::setViewClear(view_id, clear_flags, 1.0F, 0, palette, 1);
    bgfx::setViewTransform(view_id, view.data(), projection.data());
    bgfx::touch(view_id);
}

void PlanarReflectionPass::release() noexcept
{
    ENGINE_PROFILE_FUNCTION();
    if (bgfx::isValid(framebuffer_))
    {
        if (attached_view_ != UINT16_MAX)
        {
            bgfx::setViewFrameBuffer(attached_view_, BGFX_INVALID_HANDLE);
        }
        bgfx::destroy(framebuffer_);
        framebuffer_ = BGFX_INVALID_HANDLE;
    }
    if (bgfx::isValid(depth_framebuffer_))
    {
        if (attached_depth_view_ != UINT16_MAX)
        {
            bgfx::setViewFrameBuffer(attached_depth_view_, BGFX_INVALID_HANDLE);
        }
        bgfx::destroy(depth_framebuffer_);
        depth_framebuffer_ = BGFX_INVALID_HANDLE;
    }
    for (auto* handle : {&color_, &specular_, &depth_})
    {
        if (bgfx::isValid(*handle))
        {
            bgfx::destroy(*handle);
            *handle = BGFX_INVALID_HANDLE;
        }
    }
    width = height = 0;
    node_id = 0;
    attached_view_ = UINT16_MAX;
    attached_depth_view_ = UINT16_MAX;
    samples_ = 1;
}
std::uint32_t PlanarReflectionPass::resource_count() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return static_cast<std::uint32_t>(bgfx::isValid(color_)) +
           static_cast<std::uint32_t>(bgfx::isValid(specular_)) +
           static_cast<std::uint32_t>(bgfx::isValid(depth_)) +
           static_cast<std::uint32_t>(bgfx::isValid(framebuffer_)) +
           static_cast<std::uint32_t>(bgfx::isValid(depth_framebuffer_));
}
} // namespace engine::render_detail
