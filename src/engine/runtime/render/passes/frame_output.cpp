#include "runtime/render/passes/frame_output.hpp"
#include "runtime/performance/profiling.hpp"
#include "runtime/render/render_constants.hpp"

#include "fs_hdr_copy.bin.h"
#include "vs_hdr.bin.h"

#include <array>

namespace engine::render_detail
{
FrameOutput::~FrameOutput() noexcept
{
    ENGINE_PROFILE_FUNCTION();
    release();
}
bool FrameOutput::prepare(std::uint16_t width, std::uint16_t height)
{
    ENGINE_PROFILE_FUNCTION();
    if (width == 0 || height == 0)
    {
        return false;
    }
    if (!bgfx::isValid(program_))
    {
        const auto vs = bgfx::createShader(bgfx::copy(vs_hdr, sizeof(vs_hdr)));
        const auto fs = bgfx::createShader(bgfx::copy(fs_hdr_copy, sizeof(fs_hdr_copy)));
        if (!bgfx::isValid(vs) || !bgfx::isValid(fs))
        {
            if (bgfx::isValid(vs))
            {
                bgfx::destroy(vs);
            }
            if (bgfx::isValid(fs))
            {
                bgfx::destroy(fs);
            }
            return false;
        }
        program_ = bgfx::createProgram(vs, fs, true);
        sampler_ = bgfx::createUniform("s_hdr_scene", bgfx::UniformType::Sampler);
        struct Vertex
        {
            float x, y, z, u, v;
        };
        std::array vertices = {Vertex{-1, -1, 0, 0, 1}, Vertex{3, -1, 0, 2, 1},
                               Vertex{-1, 3, 0, 0, -1}};
        if (bgfx::getCaps()->originBottomLeft)
        {
            for (auto& value : vertices)
            {
                value.v = 1 - value.v;
            }
        }
        bgfx::VertexLayout layout;
        layout.begin()
            .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
            .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
            .end();
        vertices_ = bgfx::createVertexBuffer(bgfx::copy(vertices.data(), sizeof(vertices)), layout);
    }
    if (!bgfx::isValid(program_) || !bgfx::isValid(sampler_) || !bgfx::isValid(vertices_))
    {
        return false;
    }
    if (width != width_ || height != height_ || !bgfx::isValid(target_))
    {
        release_target();
        color_ =
            bgfx::createTexture2D(width, height, false, 1, bgfx::TextureFormat::BGRA8,
                                  BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
        if (!bgfx::isValid(color_))
        {
            target_ = BGFX_INVALID_HANDLE;
            return false;
        }
        if (memory_)
        {
            GpuMemoryEntry entry;
            entry.kind = GpuMemoryKind::frame_output;
            entry.logical_bytes = std::uint64_t{width} * height * 4;
            color_memory_.track(memory_, std::move(entry));
        }
        target_ = bgfx::createFrameBuffer(1, &color_, false);
        if (!bgfx::isValid(target_))
        {
            release_target();
            return false;
        }
        width_ = width;
        height_ = height;
    }
    return bgfx::isValid(target_);
}

void FrameOutput::clear(std::uint32_t clear_color) const
{
    ENGINE_PROFILE_FUNCTION();
    bgfx::setViewName(output_clear_view, "Display output clear");
    bgfx::setViewFrameBuffer(output_clear_view, target_);
    bgfx::setViewRect(output_clear_view, 0, 0, width_, height_);
    bgfx::setViewClear(output_clear_view, BGFX_CLEAR_COLOR, clear_color);
    bgfx::touch(output_clear_view);
}

void FrameOutput::bind_ui() const
{
    ENGINE_PROFILE_FUNCTION();
    bgfx::setViewFrameBuffer(ui_view, target_);
}

void FrameOutput::present() const
{
    ENGINE_PROFILE_FUNCTION();
    bgfx::setViewName(output_present_view, "Present display output");
    bgfx::setViewFrameBuffer(output_present_view, BGFX_INVALID_HANDLE);
    bgfx::setViewRect(output_present_view, 0, 0, width_, height_);
    bgfx::setViewClear(output_present_view, BGFX_CLEAR_NONE);
    bgfx::setViewTransform(output_present_view, nullptr, nullptr);
    bgfx::setTexture(0, sampler_, color_,
                     BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP | BGFX_SAMPLER_MIN_POINT |
                         BGFX_SAMPLER_MAG_POINT);
    bgfx::setVertexBuffer(0, vertices_);
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
    bgfx::submit(output_present_view, program_);
}

bool FrameOutput::capture(SceneScopeId scope, std::uint64_t scene)
{
    ENGINE_PROFILE_FUNCTION();
    if (pending_ || !bgfx::isValid(color_) ||
        !bgfx::isTextureValid(0, false, 1, bgfx::TextureFormat::BGRA8,
                              BGFX_TEXTURE_READ_BACK | BGFX_TEXTURE_BLIT_DST))
    {
        return false;
    }
    release_readback();
    readback_ = bgfx::createTexture2D(width_, height_, false, 1, bgfx::TextureFormat::BGRA8,
                                      BGFX_TEXTURE_READ_BACK | BGFX_TEXTURE_BLIT_DST);
    if (!bgfx::isValid(readback_))
    {
        return false;
    }
    capture_width_ = width_;
    capture_height_ = height_;
    const auto capture_bytes = static_cast<std::size_t>(width_) * height_ * 4;
    if (memory_)
    {
        GpuMemoryEntry entry;
        entry.kind = GpuMemoryKind::frame_readback;
        entry.creation_scope = scope;
        entry.creation_scene = scene;
        entry.logical_bytes = capture_bytes;
        readback_memory_.track(memory_, std::move(entry));
    }
    const bool reallocates = capture_bytes > pixels_.capacity();
    if (reallocates)
    {
        pixels_allocation_.reset();
    }
    pixels_.resize(capture_bytes);
    if (reallocates)
    {
        pixels_allocation_.track(memory_profile::Pool::frame_readback, pixels_.data(),
                                 pixels_.capacity());
    }
    bgfx::blit(output_capture_view, bgfx::TextureRegion{.handle = readback_},
               bgfx::TextureRegion{.handle = color_});
    pending_ = readback_completion_.submit(bgfx::TextureRegion{.handle = readback_}, pixels_.data());
    if (pending_)
    {
        readback_memory_.wait_for_readback();
    }
    else
    {
        release_readback();
    }
    return pending_;
}

void FrameOutput::retire_capture() noexcept
{
    ENGINE_PROFILE_FUNCTION();
    readback_memory_.release_requested();
}

GpuReadbackStatus FrameOutput::collect(std::uint32_t frame)
{
    ENGINE_PROFILE_FUNCTION();
    if (!pending_)
    {
        return GpuReadbackStatus::pending;
    }
    const auto result = readback_completion_.status(frame);
    if (result == GpuReadbackStatus::pending)
    {
        return result;
    }
    pending_ = false;
    readback_completion_.reset();
    if (result == GpuReadbackStatus::complete)
    {
        readback_memory_.readback_ready();
    }
    release_readback();
    return result;
}

void FrameOutput::release_target() noexcept
{
    ENGINE_PROFILE_FUNCTION();
    if (bgfx::isValid(target_))
    {
        bgfx::destroy(target_);
        target_ = BGFX_INVALID_HANDLE;
    }
    if (bgfx::isValid(color_))
    {
        bgfx::destroy(color_);
        color_ = BGFX_INVALID_HANDLE;
    }
    color_memory_.reset();
}

void FrameOutput::release_readback() noexcept
{
    ENGINE_PROFILE_FUNCTION();
    if (bgfx::isValid(readback_))
    {
        bgfx::destroy(readback_);
        readback_ = BGFX_INVALID_HANDLE;
    }
    readback_memory_.reset();
}

void FrameOutput::release() noexcept
{
    ENGINE_PROFILE_FUNCTION();
    if constexpr (web_readbacks)
    {
        readback_completion_.reset();
        pending_ = false;
    }
    release_target();
    release_readback();
    if (bgfx::isValid(program_))
    {
        bgfx::destroy(program_);
    }
    if (bgfx::isValid(sampler_))
    {
        bgfx::destroy(sampler_);
    }
    if (bgfx::isValid(vertices_))
    {
        bgfx::destroy(vertices_);
    }
    program_ = BGFX_INVALID_HANDLE;
    sampler_ = BGFX_INVALID_HANDLE;
    vertices_ = BGFX_INVALID_HANDLE;
    width_ = height_ = 0;
    // On native platforms pixels_ remains alive through bgfx shutdown.
}
} // namespace engine::render_detail
