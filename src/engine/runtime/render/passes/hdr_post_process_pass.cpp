#include "runtime/render/passes/hdr_post_process_pass.hpp"

#include "runtime/performance/function_profile.hpp"
#include "runtime/performance/profiling.hpp"
#include "runtime/render/passes/frame_output.hpp"
#include "runtime/render/passes/hdr_graph.hpp"
#include "runtime/render/render_color.hpp"
#include "runtime/render/render_constants.hpp"
#include "runtime/render/render_sampling.hpp"
#include "runtime/render/render_settings.hpp"

#include <SDL3/SDL.h>

#include "fs_hdr_blur.bin.h"
#include "fs_hdr_bright.bin.h"
#include "fs_hdr_copy.bin.h"
#include "fs_hdr_resolve.bin.h"
#include "vs_hdr.bin.h"

#include <algorithm>
#include <cmath>

namespace engine::render_detail
{
namespace
{

constexpr std::array hdr_view_names = {"Bloom bright extraction", "Bloom vertical blur",
                                       "HDR resolve and bloom horizontal blur"};
constexpr std::array hdr_views = {hdr_bright_view, hdr_blur_view, hdr_resolve_view};
constexpr std::uint8_t clear_palette = 0;
constexpr std::uint32_t linear_clamp = BGFX_SAMPLER_UVW_CLAMP;
constexpr std::uint64_t color_flags = BGFX_TEXTURE_RT | linear_clamp;

bool supports_hdr(const bgfx::Caps& caps, std::uint16_t width, std::uint16_t height,
                  std::uint8_t samples)
{
    ENGINE_PROFILE_FUNCTION();
    constexpr auto color_capabilities =
        BGFX_CAPS_FORMAT_TEXTURE_2D | BGFX_CAPS_FORMAT_TEXTURE_FRAMEBUFFER;
    if (!valid_msaa_samples(samples) || width == 0 || height == 0 ||
        width > caps.limits.maxTextureSize || height > caps.limits.maxTextureSize ||
        caps.limits.maxTextureSamplers < 2 || caps.limits.maxFBAttachments < 3)
    {
        SDL_LogError(SDL_LOG_CATEGORY_RENDER,
                     "HDR target dimensions or sampler limits are invalid (%u x %u, max=%u)",
                     static_cast<unsigned int>(width), static_cast<unsigned int>(height),
                     caps.limits.maxTextureSize);
        return false;
    }
    if ((caps.formats[bgfx::TextureFormat::RGBA16F] & color_capabilities) != color_capabilities ||
        !bgfx::isTextureValid(0, false, 1, bgfx::TextureFormat::RGBA16F,
                              render_target_flags(samples) | linear_clamp) ||
        !bgfx::isTextureValid(0, false, 1, bgfx::TextureFormat::D32F,
                              render_target_flags(samples) | BGFX_TEXTURE_RT_WRITE_ONLY))
    {
        SDL_LogError(SDL_LOG_CATEGORY_RENDER,
                     "HDR requires sampled RGBA16F and D32F render targets (color=0x%x depth=0x%x)",
                     static_cast<unsigned int>(caps.formats[bgfx::TextureFormat::RGBA16F]),
                     static_cast<unsigned int>(caps.formats[bgfx::TextureFormat::D32F]));
        return false;
    }
    return true;
}

bool valid_settings(const RenderSettings& settings)
{
    ENGINE_PROFILE_FUNCTION();
    return std::isfinite(settings.exposure) && settings.exposure > 0.0F &&
           settings.exposure <= 100.0F && std::isfinite(settings.bloom_threshold) &&
           settings.bloom_threshold >= 0.0F && settings.bloom_threshold <= 100.0F &&
           std::isfinite(settings.bloom_intensity) && settings.bloom_intensity >= 0.0F &&
           settings.bloom_intensity <= 10.0F && std::isfinite(settings.bloom_radius) &&
           settings.bloom_radius >= 0.25F && settings.bloom_radius <= 4.0F;
}

void set_post_view_state(bgfx::ViewId view)
{
    bgfx::setViewClear(view, BGFX_CLEAR_NONE);
    bgfx::setViewTransform(view, nullptr, nullptr);
    bgfx::setViewMode(view, bgfx::ViewMode::Sequential);
}

void set_post_view(bgfx::ViewId view, const char* name, bgfx::FrameBufferHandle framebuffer,
                   std::uint16_t width, std::uint16_t height)
{
    ENGINE_PROFILE_FUNCTION();
    bgfx::setViewName(view, name);
    bgfx::setViewFrameBuffer(view, framebuffer);
    bgfx::setViewRect(view, 0, 0, width, height);
    set_post_view_state(view);
}

} // namespace

HdrPostProcessPass::~HdrPostProcessPass() noexcept
{
    ENGINE_PROFILE_FUNCTION();
    release();
}

void HdrPostProcessPass::initialize_views()
{
    // These three reserved views retain their state until device shutdown. Swap-chain resets
    // invalidate framebuffer bindings, which each graph segment supplies on submission.
    if (bgfx::getCaps()->limits.maxViews > hdr_resolve_view)
    {
        for (std::size_t i = 0; i < hdr_views.size(); ++i)
        {
            bgfx::setViewName(hdr_views[i], hdr_view_names[i]);
            set_post_view_state(hdr_views[i]);
        }
    }
}

bool HdrPostProcessPass::initialize_resources()
{
    ENGINE_PROFILE_FUNCTION();
    if (std::all_of(programs_.begin(), programs_.end(),
                    [](auto handle)
                    {
                        ENGINE_PROFILE_FUNCTION_PROBE();
                        return bgfx::isValid(handle);
                    }) &&
        bgfx::isValid(fullscreen_) && bgfx::isValid(black_) && bgfx::isValid(parameters_) &&
        bgfx::isValid(texel_) && bgfx::isValid(output_))
    {
        return true;
    }
    release();
    // Frame ordering and fullscreen sampling adapt bgfx examples/09-hdr at
    // 7a70927d2af7ffcc5cef9c9e8f4c821e31fcb099. Keep radiance in RGBA16F rather than RGBE8.
    // Copyright 2011-2025 Branimir Karadzic; BSD-2-Clause, third_party/notices/bgfx-hdr.md.
    struct Bytecode
    {
        const std::uint8_t* data;
        std::uint32_t size;
    };
    const std::array fragments = {
        Bytecode{fs_hdr_bright, static_cast<std::uint32_t>(sizeof(fs_hdr_bright))},
        Bytecode{fs_hdr_blur, static_cast<std::uint32_t>(sizeof(fs_hdr_blur))},
        Bytecode{fs_hdr_resolve, static_cast<std::uint32_t>(sizeof(fs_hdr_resolve))},
        Bytecode{fs_hdr_copy, static_cast<std::uint32_t>(sizeof(fs_hdr_copy))}};
    const auto vertex =
        bgfx::createShader(bgfx::copy(vs_hdr, static_cast<std::uint32_t>(sizeof(vs_hdr))));
    if (!bgfx::isValid(vertex))
    {
        return false;
    }
    bool complete = true;
    for (std::size_t index = 0; index < programs_.size(); ++index)
    {
        const auto fragment =
            bgfx::createShader(bgfx::copy(fragments[index].data, fragments[index].size));
        if (!bgfx::isValid(fragment))
        {
            complete = false;
            break;
        }
        programs_[index] = bgfx::createProgram(vertex, fragment, false);
        bgfx::destroy(fragment);
        if (!bgfx::isValid(programs_[index]))
        {
            complete = false;
            break;
        }
    }
    bgfx::destroy(vertex);
    if (!complete)
    {
        return false;
    }
    constexpr std::array sampler_names = {"s_hdr_scene", "s_hdr_bloom"};
    for (std::size_t index = 0; index < samplers_.size(); ++index)
    {
        samplers_[index] = bgfx::createUniform(sampler_names[index], bgfx::UniformType::Sampler);
        if (!bgfx::isValid(samplers_[index]))
        {
            return false;
        }
    }
    parameters_ = bgfx::createUniform("u_hdr_params", bgfx::UniformType::Vec4);
    texel_ = bgfx::createUniform("u_hdr_texel", bgfx::UniformType::Vec4);
    output_ = bgfx::createUniform("u_hdr_output", bgfx::UniformType::Vec4);
    struct Vertex
    {
        float x;
        float y;
        float z;
        float u;
        float v;
    };
    std::array vertices = {Vertex{-1.0F, -1.0F, 0.0F, 0.0F, 1.0F},
                           Vertex{3.0F, -1.0F, 0.0F, 2.0F, 1.0F},
                           Vertex{-1.0F, 3.0F, 0.0F, 0.0F, -1.0F}};
    if (bgfx::getCaps()->originBottomLeft)
    {
        for (auto& vertex_data : vertices)
        {
            vertex_data.v = 1.0F - vertex_data.v;
        }
    }
    bgfx::VertexLayout layout;
    layout.begin()
        .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
        .end();
    fullscreen_ = bgfx::createVertexBuffer(bgfx::copy(vertices.data(), sizeof(vertices)), layout);
    constexpr std::uint32_t black = 0;
    black_ = bgfx::createTexture2D(1, 1, false, 1, bgfx::TextureFormat::RGBA8, linear_clamp,
                                   bgfx::copy(&black, sizeof(black)));
    return bgfx::isValid(parameters_) && bgfx::isValid(texel_) && bgfx::isValid(output_) &&
           bgfx::isValid(fullscreen_) && bgfx::isValid(black_);
}

bool HdrPostProcessPass::prepare(std::uint16_t width, std::uint16_t height, bool bloom_enabled,
                                 std::uint8_t samples, bool ssr_enabled)
{
    ENGINE_PROFILE_ZONE("Render.hdrPrepare");
    const auto* caps = bgfx::getCaps();
    if (caps == nullptr || !supports_hdr(*caps, width, height, samples))
    {
        release();
        return false;
    }
    if (!initialize_resources())
    {
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Cannot initialize HDR post-process programs");
        release();
        return false;
    }
    const bool reuse_opaque_color = ssr_enabled && samples > 1;
    if (width_ != width || height_ != height || samples_ != samples ||
        reuse_opaque_color_ != reuse_opaque_color || !bgfx::isValid(framebuffer_))
    {
        release_targets();
        width_ = width;
        height_ = height;
        samples_ = samples;
        reuse_opaque_color_ = reuse_opaque_color;
        const auto scene_color_flags = render_target_flags(samples) | linear_clamp;
        color_ = bgfx::createTexture2D(width, height, false, 1, bgfx::TextureFormat::RGBA16F,
                                       scene_color_flags);
        specular_ = bgfx::createTexture2D(width, height, false, 1, bgfx::TextureFormat::RGBA16F,
                                          scene_color_flags);
        if (!reuse_opaque_color_)
        {
            final_color_ = bgfx::createTexture2D(
                width, height, false, 1, bgfx::TextureFormat::RGBA16F,
                scene_color_flags | (samples == 1 ? BGFX_TEXTURE_BLIT_DST : 0));
        }
        const auto depth_flags = render_target_flags(samples) | BGFX_TEXTURE_RT_WRITE_ONLY;
        if (reuse_opaque_color_ &&
            bgfx::isTextureValid(0, false, 1, bgfx::TextureFormat::D32FS8, depth_flags))
        {
            depth_ = bgfx::createTexture2D(width, height, false, 1, bgfx::TextureFormat::D32FS8,
                                           depth_flags);
            if (bgfx::isValid(depth_))
            {
                depth_framebuffer_ = bgfx::createFrameBuffer(1, &depth_, false);
                if (!bgfx::isValid(depth_framebuffer_))
                {
                    bgfx::destroy(depth_);
                    depth_ = BGFX_INVALID_HANDLE;
                }
            }
        }
        if (!bgfx::isValid(depth_))
        {
            depth_ = bgfx::createTexture2D(width, height, false, 1, bgfx::TextureFormat::D32F,
                                           depth_flags);
        }
        if (bgfx::isValid(color_) && bgfx::isValid(depth_) && bgfx::isValid(specular_) &&
            bgfx::isValid(scene_color()))
        {
            std::array<bgfx::Attachment, 3> attachments{};
            // SSR reads both resolves. Transparency initializes the reused color samples.
            const auto opaque_attachment_flags =
                reuse_opaque_color_ ? BGFX_ATTACHMENT_RESOLVE_ONLY : BGFX_ATTACHMENT_NONE;
            attachments[0].init(color_, bgfx::Access::Write, 0, 1, 0, opaque_attachment_flags);
            attachments[1].init(specular_, bgfx::Access::Write, 0, 1, 0, opaque_attachment_flags);
            attachments[2].init(depth_, bgfx::Access::Write, 0, 1, 0, BGFX_ATTACHMENT_NONE);
            framebuffer_ = bgfx::createFrameBuffer(static_cast<std::uint8_t>(attachments.size()),
                                                   attachments.data(), false);
            // SSR has finished reading the opaque colors by the time this view writes them.
            const std::array final_attachments = {scene_color(), depth_};
            final_framebuffer_ = bgfx::createFrameBuffer(2, final_attachments.data(), false);
        }
        if (!bgfx::isValid(framebuffer_) || !bgfx::isValid(final_framebuffer_))
        {
            SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Cannot allocate HDR scene target");
            release();
            return false;
        }
    }
    if (!bloom_enabled)
    {
        release_bloom();
        return true;
    }
    if (std::all_of(bloom_framebuffers_.begin(), bloom_framebuffers_.end(),
                    [](auto handle)
                    {
                        ENGINE_PROFILE_FUNCTION_PROBE();
                        return bgfx::isValid(handle);
                    }))
    {
        return true;
    }
    release_bloom();
    constexpr std::array<std::uint32_t, 2> divisors{2, 8};
    for (std::size_t index = 0; index < bloom_colors_.size(); ++index)
    {
        bloom_widths_[index] = static_cast<std::uint16_t>(
            (static_cast<std::uint32_t>(width) + divisors[index] - 1) / divisors[index]);
        bloom_heights_[index] = static_cast<std::uint16_t>(
            (static_cast<std::uint32_t>(height) + divisors[index] - 1) / divisors[index]);
        bloom_colors_[index] =
            bgfx::createTexture2D(bloom_widths_[index], bloom_heights_[index], false, 1,
                                  bgfx::TextureFormat::RGBA16F, color_flags);
        if (bgfx::isValid(bloom_colors_[index]))
        {
            bloom_framebuffers_[index] = bgfx::createFrameBuffer(1, &bloom_colors_[index], false);
        }
        if (!bgfx::isValid(bloom_framebuffers_[index]))
        {
            SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Cannot allocate HDR bloom targets");
            release();
            return false;
        }
    }
    return true;
}

bool HdrPostProcessPass::begin_opaque_depth(const float* view, const float* projection)
{
    ENGINE_PROFILE_FUNCTION();
    if (!bgfx::isValid(depth_framebuffer_))
    {
        return false;
    }
    bgfx::setViewName(opaque_depth_view, "Opaque depth prepass");
    bgfx::setViewFrameBuffer(opaque_depth_view, depth_framebuffer_);
    bgfx::setViewRect(opaque_depth_view, 0, 0, width_, height_);
    bgfx::setViewTransform(opaque_depth_view, view, projection);
    bgfx::setViewClear(opaque_depth_view, BGFX_CLEAR_DEPTH | BGFX_CLEAR_STENCIL, 0, 1.0F, 0);
    bgfx::touch(opaque_depth_view);
    return true;
}

void HdrPostProcessPass::begin_scene(std::uint32_t clear_color, bool depth_prepared)
{
    ENGINE_PROFILE_FUNCTION();
    std::array<float, 4> clear{};
    for (std::size_t index = 0; index < 4; ++index)
    {
        const auto shift = static_cast<unsigned int>((3 - index) * 8);
        clear[index] = static_cast<float>((clear_color >> shift) & 0xffU) / 255.0F;
        if (index != 3)
        {
            clear[index] = display_srgb_to_linear(clear[index]);
        }
    }
    clear[3] = 0.0F;
    bgfx::setPaletteColor(clear_palette, clear.data());
    constexpr float black[] = {0, 0, 0, 0};
    bgfx::setPaletteColor(1, black);
    bgfx::setViewName(main_view, "Opaque scene and environment specular");
    bgfx::setViewFrameBuffer(main_view, framebuffer_);
    bgfx::setViewRect(main_view, 0, 0, width_, height_);
    const auto clear_flags = BGFX_CLEAR_COLOR | (depth_prepared ? 0 : BGFX_CLEAR_DEPTH);
    bgfx::setViewClear(main_view, clear_flags, 1.0F, 0, clear_palette, 1);
    bgfx::touch(main_view);
}

bgfx::FrameBufferHandle HdrPostProcessPass::scene_framebuffer() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return final_framebuffer_;
}

void HdrPostProcessPass::begin_transparency(bgfx::TextureHandle frozen_scene, const float* view,
                                            const float* projection)
{
    ENGINE_PROFILE_ZONE("Render.transparentSceneCopySubmit");
    bgfx::setViewName(scene_copy_view, "Freeze opaque scene for refraction");
    if (samples_ == 1)
    {
        // A previous multisampled frame may have attached this view to its old target.
        bgfx::setViewFrameBuffer(scene_copy_view, BGFX_INVALID_HANDLE);
        bgfx::blit(scene_copy_view, bgfx::TextureRegion{.handle = scene_color()},
                   bgfx::TextureRegion{.handle = frozen_scene});
    }
    else
    {
        // A blit only writes a texture's resolved image, not its multisample attachment.
        // Populate every color sample through a draw, preserving the opaque MSAA depth.
        set_post_view(scene_copy_view, "Initialize transparent MSAA scene", final_framebuffer_,
                      width_, height_);
        if (reuse_opaque_color_)
        {
            bgfx::setViewClear(scene_copy_view, BGFX_CLEAR_COLOR, 0, 1.0F, 0);
        }
        bgfx::setTexture(0, samplers_[0], frozen_scene, linear_clamp);
        bgfx::setVertexBuffer(0, fullscreen_);
        bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_MSAA);
        bgfx::submit(scene_copy_view, programs_[3]);
    }
    bgfx::setViewName(transparent_view, "Sorted transparent surfaces");
    bgfx::setViewFrameBuffer(transparent_view, final_framebuffer_);
    bgfx::setViewRect(transparent_view, 0, 0, width_, height_);
    bgfx::setViewClear(transparent_view, BGFX_CLEAR_NONE);
    bgfx::setViewTransform(transparent_view, view, projection);
    bgfx::setViewMode(transparent_view, bgfx::ViewMode::Sequential);
    bgfx::touch(transparent_view);
}

bool HdrPostProcessPass::resolve(const RenderSettings& settings, const FrameOutput& display,
                                 RenderGraph* graph)
{
    ENGINE_PROFILE_ZONE("Render.hdrResolveSubmit");
    if (!valid_settings(settings) || width_ == 0 || height_ == 0 || !bgfx::isValid(framebuffer_) ||
        display.width() != width_ || display.height() != height_ ||
        !bgfx::isValid(display.target()) ||
        (settings.bloom_enabled &&
         !std::all_of(bloom_framebuffers_.begin(), bloom_framebuffers_.end(),
                      [](auto handle)
                      {
                          ENGINE_PROFILE_FUNCTION_PROBE();
                          return bgfx::isValid(handle);
                      })))
    {
        SDL_LogError(SDL_LOG_CATEGORY_RENDER,
                     "HDR resolve requires prepared targets and valid settings");
        return false;
    }
    const float parameters[] = {settings.exposure, settings.filmic_tonemap ? 1.0F : 0.0F,
                                settings.bloom_enabled ? settings.bloom_intensity : 0.0F,
                                settings.bloom_threshold};
    std::array<float, 4> bloom_texel{};
    if (settings.bloom_enabled)
    {
        bloom_texel = {settings.bloom_radius / bloom_widths_[1],
                       settings.bloom_radius / bloom_heights_[1], 0.0F, 0.0F};
    }
    struct Target
    {
        bgfx::FrameBufferHandle framebuffer;
        std::uint16_t width;
        std::uint16_t height;
    };
    const Target bright{bloom_framebuffers_[0], bloom_widths_[0], bloom_heights_[0]};
    const Target blur{bloom_framebuffers_[1], bloom_widths_[1], bloom_heights_[1]};
    const Target output{display.target(), width_, height_};
#if defined(ENGINE_ENABLE_TEST_SUPPORT)
    if (!graph)
    {
#if defined(ENGINE_ENABLE_TRACY_FUNCTIONS)
        ENGINE_PROFILE_ZONE("Render.hdrLegacySubmit");
#endif
        const auto submit_legacy = [&](GraphPass pass, bgfx::ViewId view, const Target& target,
                                       bgfx::TextureHandle source, bgfx::TextureHandle bloom)
        {
            set_post_view(view, hdr_view_names[pass], target.framebuffer, target.width,
                          target.height);
            const float output_parameters[]{settings.hdr_enabled ? 1.0F : 0.0F, 0, 0, 0};
            const auto texel = pass == HdrPass::bright
                                   ? std::array{1.0F / width_, 1.0F / height_, 0.0F, 0.0F}
                                   : bloom_texel;
            bgfx::setUniform(output_, output_parameters);
            bgfx::setUniform(parameters_, parameters);
            bgfx::setUniform(texel_, texel.data());
            bgfx::setTexture(0, samplers_[0], source, linear_clamp);
            if (pass == HdrPass::resolve)
            {
                bgfx::setTexture(1, samplers_[1], bloom, linear_clamp);
            }
            bgfx::setVertexBuffer(0, fullscreen_);
            bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
            bgfx::submit(view, programs_[pass]);
        };
        if (settings.bloom_enabled)
        {
            submit_legacy(HdrPass::bright, hdr_bright_view, bright, scene_color(),
                          BGFX_INVALID_HANDLE);
            submit_legacy(HdrPass::blur, hdr_blur_view, blur, bloom_colors_[0],
                          BGFX_INVALID_HANDLE);
        }
        submit_legacy(HdrPass::resolve, hdr_resolve_view, output, scene_color(),
                      settings.bloom_enabled ? bloom_colors_[1] : black_);
        return true;
    }
#endif
    FrameBindings bindings;
    {
#if defined(ENGINE_ENABLE_TRACY_FUNCTIONS)
        ENGINE_PROFILE_ZONE("Render.graphDeclare");
#endif
        // The device-wide order in RenderSystem contains 256 physical views.
        const auto max_views = static_cast<std::uint16_t>(
            std::min(bgfx::getCaps()->limits.maxViews, std::uint32_t{256}));
        if (!graph->begin({width_, height_, max_views, samples_}))
        {
            SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Cannot start the HDR render graph");
            return false;
        }
        bindings.textures[HdrSlot::scene] = scene_color().idx;
        bindings.textures[HdrSlot::black] = black_.idx;
        bindings.textures[HdrSlot::bright] = bloom_colors_[0].idx;
        bindings.textures[HdrSlot::blur] = bloom_colors_[1].idx;
        bindings.textures[HdrSlot::output] = display.color().idx;
    }
    GraphError error;
    if (compile_hdr_graph(*graph, settings.bloom_enabled, bindings, error))
    {
#if defined(ENGINE_ENABLE_TRACY_FUNCTIONS)
        ENGINE_PROFILE_ZONE("Render.graphExecute");
#endif
        const std::array<Target, HdrSlot::count> targets{{{}, {}, bright, blur, output}};
        const auto passes = graph->plan().passes();
        const auto encode = [&](const GraphPassDesc& segment)
        {
            const auto& target = targets[segment.output];
            bgfx::setViewFrameBuffer(segment.view, target.framebuffer);
            bgfx::setViewRect(segment.view, 0, 0, target.width, target.height);
            const auto texel = segment.pass == HdrPass::bright
                                   ? std::array{1.0F / width_, 1.0F / height_, 0.0F, 0.0F}
                                   : bloom_texel;
            bgfx::setUniform(texel_, texel.data());
            if (segment.pass != HdrPass::blur)
            {
                bgfx::setUniform(parameters_, parameters);
            }
            const bgfx::TextureHandle source{bindings.textures[segment.reads[0]]};
            bgfx::setTexture(0, samplers_[0], source, linear_clamp);
            if (segment.pass == HdrPass::resolve)
            {
                const float output_parameters[]{settings.hdr_enabled ? 1.0F : 0.0F, 0, 0, 0};
                bgfx::setUniform(output_, output_parameters);
                const bgfx::TextureHandle bloom{bindings.textures[segment.reads[1]]};
                bgfx::setTexture(1, samplers_[1], bloom, linear_clamp);
            }
            if (segment.pass == passes.front().pass)
            {
                bgfx::setVertexBuffer(0, fullscreen_);
            }
            // DISCARD_STATE also advances the per-draw uniform range. Keep only geometry
            // between these draws so each shader gets exactly its own uniform commands.
            constexpr std::uint8_t keep_geometry = BGFX_DISCARD_ALL & ~BGFX_DISCARD_VERTEX_STREAMS;
            const auto discard =
                segment.pass == passes.back().pass ? BGFX_DISCARD_ALL : keep_geometry;
            bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
            bgfx::submit(segment.view, programs_[segment.pass], 0, discard);
            return true;
        };
        if (graph->execute(bindings, encode, error))
        {
            return true;
        }
    }
    SDL_LogError(SDL_LOG_CATEGORY_RENDER, "HDR graph failed on %s (pass=%u, slot=%u): %s",
                 bgfx::getRendererName(bgfx::getRendererType()), static_cast<unsigned>(error.pass),
                 static_cast<unsigned>(error.slot), error.reason);
    return false;
}

std::uint32_t HdrPostProcessPass::resource_count() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    std::uint32_t count = static_cast<std::uint32_t>(bgfx::isValid(parameters_)) +
                          static_cast<std::uint32_t>(bgfx::isValid(texel_)) +
                          static_cast<std::uint32_t>(bgfx::isValid(output_)) +
                          static_cast<std::uint32_t>(bgfx::isValid(fullscreen_)) +
                          static_cast<std::uint32_t>(bgfx::isValid(black_)) +
                          static_cast<std::uint32_t>(bgfx::isValid(color_)) +
                          static_cast<std::uint32_t>(bgfx::isValid(specular_)) +
                          static_cast<std::uint32_t>(bgfx::isValid(final_color_)) +
                          static_cast<std::uint32_t>(bgfx::isValid(final_framebuffer_)) +
                          static_cast<std::uint32_t>(bgfx::isValid(depth_)) +
                          static_cast<std::uint32_t>(bgfx::isValid(depth_framebuffer_)) +
                          static_cast<std::uint32_t>(bgfx::isValid(framebuffer_));
    for (auto handle : programs_)
    {
        count += static_cast<std::uint32_t>(bgfx::isValid(handle));
    }
    for (auto handle : samplers_)
    {
        count += static_cast<std::uint32_t>(bgfx::isValid(handle));
    }
    for (auto handle : bloom_colors_)
    {
        count += static_cast<std::uint32_t>(bgfx::isValid(handle));
    }
    for (auto handle : bloom_framebuffers_)
    {
        count += static_cast<std::uint32_t>(bgfx::isValid(handle));
    }
    return count;
}

void HdrPostProcessPass::release_bloom() noexcept
{
    ENGINE_PROFILE_FUNCTION();
    for (std::size_t index = 0; index < bloom_framebuffers_.size(); ++index)
    {
        if (bgfx::isValid(bloom_framebuffers_[index]))
        {
            bgfx::setViewFrameBuffer(static_cast<bgfx::ViewId>(hdr_bright_view + index),
                                     BGFX_INVALID_HANDLE);
            bgfx::destroy(bloom_framebuffers_[index]);
            bloom_framebuffers_[index] = BGFX_INVALID_HANDLE;
        }
        if (bgfx::isValid(bloom_colors_[index]))
        {
            bgfx::destroy(bloom_colors_[index]);
            bloom_colors_[index] = BGFX_INVALID_HANDLE;
        }
    }
    bloom_widths_ = {};
    bloom_heights_ = {};
}

void HdrPostProcessPass::release_targets() noexcept
{
    ENGINE_PROFILE_FUNCTION();
    release_bloom();
    if (bgfx::isValid(depth_framebuffer_))
    {
        bgfx::setViewFrameBuffer(opaque_depth_view, BGFX_INVALID_HANDLE);
        bgfx::destroy(depth_framebuffer_);
        depth_framebuffer_ = BGFX_INVALID_HANDLE;
    }
    if (bgfx::isValid(framebuffer_))
    {
        bgfx::setViewFrameBuffer(main_view, BGFX_INVALID_HANDLE);
        bgfx::setViewFrameBuffer(extension_view, BGFX_INVALID_HANDLE);
        bgfx::setViewName(main_view, "Main window");
        bgfx::destroy(framebuffer_);
        framebuffer_ = BGFX_INVALID_HANDLE;
    }
    if (bgfx::isValid(final_framebuffer_))
    {
        bgfx::setViewFrameBuffer(scene_copy_view, BGFX_INVALID_HANDLE);
        bgfx::setViewFrameBuffer(transparent_view, BGFX_INVALID_HANDLE);
        bgfx::setViewFrameBuffer(extension_view, BGFX_INVALID_HANDLE);
        bgfx::destroy(final_framebuffer_);
        final_framebuffer_ = BGFX_INVALID_HANDLE;
    }
    for (auto* handle : {&color_, &specular_, &final_color_, &depth_})
    {
        if (bgfx::isValid(*handle))
        {
            bgfx::destroy(*handle);
            *handle = BGFX_INVALID_HANDLE;
        }
    }
    width_ = 0;
    height_ = 0;
    samples_ = 1;
    reuse_opaque_color_ = false;
}

void HdrPostProcessPass::release() noexcept
{
    ENGINE_PROFILE_FUNCTION();
    release_targets();
    for (auto& handle : programs_)
    {
        if (bgfx::isValid(handle))
        {
            bgfx::destroy(handle);
            handle = BGFX_INVALID_HANDLE;
        }
    }
    for (auto& handle : samplers_)
    {
        if (bgfx::isValid(handle))
        {
            bgfx::destroy(handle);
            handle = BGFX_INVALID_HANDLE;
        }
    }
    for (auto* handle : {&parameters_, &texel_, &output_})
    {
        if (bgfx::isValid(*handle))
        {
            bgfx::destroy(*handle);
            *handle = BGFX_INVALID_HANDLE;
        }
    }
    if (bgfx::isValid(fullscreen_))
    {
        bgfx::destroy(fullscreen_);
        fullscreen_ = BGFX_INVALID_HANDLE;
    }
    if (bgfx::isValid(black_))
    {
        bgfx::destroy(black_);
        black_ = BGFX_INVALID_HANDLE;
    }
}

} // namespace engine::render_detail
