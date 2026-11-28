#include "runtime/render/movie_pass.hpp"
#include "fs_movie.bin.h"
#include "runtime/performance/profiling.hpp"
#include "runtime/render/passes/frame_output.hpp"
#include "runtime/render/render_constants.hpp"
#include "vs_movie.bin.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <vector>

namespace engine::render_detail
{
namespace
{
constexpr std::uint64_t sampler_flags = BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
struct Vertex
{
    float x, y, z, u, v;
};

bool valid_plane(const MoviePlane& plane, std::uint32_t width, std::uint32_t height,
                 std::uint32_t channels)
{
    ENGINE_PROFILE_FUNCTION();
    const auto row = static_cast<std::uint64_t>(width) * channels;
    const auto size = static_cast<std::uint64_t>(plane.stride_bytes) * (height - 1) + row;
    return plane.width == width && plane.height == height && plane.stride_bytes >= row &&
           size <= plane.bytes.size();
}

bgfx::TextureHandle upload(const MoviePlane& plane, std::uint32_t channels)
{
    ENGINE_PROFILE_FUNCTION();
    const auto row = static_cast<std::size_t>(plane.width) * channels;
    const auto bytes = row * plane.height;
    if (bytes > std::numeric_limits<std::uint32_t>::max())
    {
        return BGFX_INVALID_HANDLE;
    }
    const auto* memory = bgfx::alloc(static_cast<std::uint32_t>(bytes));
    if (!memory)
    {
        return BGFX_INVALID_HANDLE;
    }
    for (std::uint32_t y = 0; y < plane.height; ++y)
    {
        std::memcpy(memory->data + row * y,
                    plane.bytes.data() + static_cast<std::size_t>(plane.stride_bytes) * y, row);
    }
    return bgfx::createTexture2D(
        static_cast<std::uint16_t>(plane.width), static_cast<std::uint16_t>(plane.height), false, 1,
        channels == 2 ? bgfx::TextureFormat::RG8 : bgfx::TextureFormat::R8, sampler_flags, memory);
}
} // namespace
PreparedMovieFrame::~PreparedMovieFrame() noexcept
{
    release();
}

void PreparedMovieFrame::release() noexcept
{
    for (auto& plane : planes_)
    {
        if (bgfx::isValid(plane))
        {
            bgfx::destroy(plane);
            plane = BGFX_INVALID_HANDLE;
        }
    }
    width_ = height_ = 0;
}

MoviePass::~MoviePass() noexcept
{
    ENGINE_PROFILE_FUNCTION();
    shutdown();
}
bool MoviePass::initialize()
{
    ENGINE_PROFILE_FUNCTION();
    if (bgfx::isValid(program_))
    {
        return true;
    }
    const auto vs = bgfx::createShader(bgfx::copy(vs_movie, sizeof(vs_movie)));
    const auto fs = bgfx::createShader(bgfx::copy(fs_movie, sizeof(fs_movie)));
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
    constexpr std::array names{"s_movie_y", "s_movie_u", "s_movie_v"};
    for (std::size_t i = 0; i < names.size(); ++i)
    {
        samplers_[i] = bgfx::createUniform(names[i], bgfx::UniformType::Sampler);
    }
    color_ = bgfx::createUniform("u_movie_color", bgfx::UniformType::Vec4);
    chroma_ = bgfx::createUniform("u_movie_chroma", bgfx::UniformType::Vec4);
    std::array vertices{Vertex{-1, -1, 0, 0, 1}, Vertex{3, -1, 0, 2, 1}, Vertex{-1, 3, 0, 0, -1}};
    if (bgfx::getCaps()->originBottomLeft)
    {
        for (auto& v : vertices)
        {
            v.v = 1 - v.v;
        }
    }
    bgfx::VertexLayout layout;
    layout.begin()
        .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
        .end();
    vertices_ = bgfx::createVertexBuffer(bgfx::copy(vertices.data(), sizeof(vertices)), layout);
    if (!bgfx::isValid(program_) || !bgfx::isValid(vertices_) || !bgfx::isValid(color_) ||
        !bgfx::isValid(chroma_) || !bgfx::isValid(samplers_[0]) || !bgfx::isValid(samplers_[1]) ||
        !bgfx::isValid(samplers_[2]))
    {
        shutdown();
        return false;
    }
    return true;
}
bool MoviePass::validate(const MovieFrame& frame, const FrameOutput& output,
                         PreparedMovieFrame& prepared)
{
    ENGINE_PROFILE_FUNCTION();
    prepared.release();
    const auto width = output.width();
    const auto height = output.height();
    if (!frame.width || !frame.height || frame.width > 65535 || frame.height > 65535 || !width ||
        !height ||
        (frame.format != schema::MoviePixelFormat::nv12 &&
         frame.format != schema::MoviePixelFormat::yuv420p8) ||
        frame.color.primaries() != schema::MovieColorPrimaries::bt709 ||
        (frame.color.transfer() != schema::MovieTransfer::srgb &&
         frame.color.transfer() != schema::MovieTransfer::bt709) ||
        (frame.color.matrix() != schema::MovieColorMatrix::bt601 &&
         frame.color.matrix() != schema::MovieColorMatrix::bt709) ||
        (frame.color.range() != schema::MovieColorRange::full &&
         frame.color.range() != schema::MovieColorRange::limited) ||
        (frame.color.chroma_location() != schema::MovieChromaLocation::left &&
         frame.color.chroma_location() != schema::MovieChromaLocation::center))
    {
        return false;
    }
    const bool nv12 = frame.format == schema::MoviePixelFormat::nv12;
    const auto cw = (frame.width + 1) / 2;
    const auto ch = (frame.height + 1) / 2;
    if (!valid_plane(frame.planes[0], frame.width, frame.height, 1) ||
        !valid_plane(frame.planes[1], cw, ch, nv12 ? 2 : 1) ||
        (!nv12 && !valid_plane(frame.planes[2], cw, ch, 1)) ||
        (nv12 && (!frame.planes[2].bytes.empty() || frame.planes[2].width != 0 ||
                  frame.planes[2].height != 0 || frame.planes[2].stride_bytes != 0)))
    {
        return false;
    }
    const auto target_width = static_cast<std::uint16_t>(std::min<std::uint64_t>(
        width, static_cast<std::uint64_t>(height) * frame.width / frame.height));
    const auto target_height = static_cast<std::uint16_t>(std::min<std::uint64_t>(
        height, static_cast<std::uint64_t>(width) * frame.height / frame.width));
    if (!target_width || !target_height)
    {
        return false;
    }

    prepared.params_ = {frame.color.matrix() == schema::MovieColorMatrix::bt601 ? 1.0F : 0.0F,
                        frame.color.range() == schema::MovieColorRange::limited ? 1.0F : 0.0F,
                        frame.color.transfer() == schema::MovieTransfer::bt709 ? 1.0F : 0.0F,
                        nv12 ? 1.0F : 0.0F};
    prepared.chroma_ = {frame.color.chroma_location() == schema::MovieChromaLocation::left
                            ? 0.25F / static_cast<float>(cw)
                            : 0.0F,
                        0, 0, 0};
    prepared.width_ = target_width;
    prepared.height_ = target_height;
    return true;
}

bool MoviePass::upload_plane(const MovieFrame& frame, std::size_t index,
                             PreparedMovieFrame& prepared)
{
    if (index >= prepared.planes_.size() || !initialize())
    {
        return false;
    }
    auto& plane = prepared.planes_[index];
    plane = upload(frame.planes[index],
                   index == 1 && frame.format == schema::MoviePixelFormat::nv12 ? 2 : 1);
    return bgfx::isValid(plane);
}

void MoviePass::draw(const PreparedMovieFrame& prepared, const FrameOutput& output) const
{
    ENGINE_PROFILE_FUNCTION();
    const auto width = output.width();
    const auto height = output.height();
    const auto target_width = prepared.width_;
    const auto target_height = prepared.height_;
    const auto& planes = prepared.planes_;
    bgfx::setViewFrameBuffer(main_view, BGFX_INVALID_HANDLE);
    bgfx::setViewRect(main_view, 0, 0, width, height);
    bgfx::setViewClear(main_view, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x000000ff, 1.0F, 0);
    bgfx::touch(main_view);
    bgfx::setViewFrameBuffer(movie_view, output.target());
    bgfx::setViewRect(movie_view, (width - target_width) / 2, (height - target_height) / 2,
                      target_width, target_height);
    bgfx::setViewClear(movie_view, BGFX_CLEAR_NONE);
    bgfx::setTexture(0, samplers_[0], planes[0]);
    bgfx::setTexture(1, samplers_[1], planes[1]);
    bgfx::setTexture(2, samplers_[2], bgfx::isValid(planes[2]) ? planes[2] : planes[1]);
    bgfx::setUniform(color_, prepared.params_.data());
    bgfx::setUniform(chroma_, prepared.chroma_.data());
    bgfx::setVertexBuffer(0, vertices_);
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
    bgfx::submit(movie_view, program_);
}
void MoviePass::shutdown() noexcept
{
    ENGINE_PROFILE_FUNCTION();
    if (bgfx::isValid(vertices_))
    {
        bgfx::destroy(vertices_);
    }
    if (bgfx::isValid(program_))
    {
        bgfx::destroy(program_);
    }
    if (bgfx::isValid(color_))
    {
        bgfx::destroy(color_);
    }
    if (bgfx::isValid(chroma_))
    {
        bgfx::destroy(chroma_);
    }
    for (auto& sampler : samplers_)
    {
        if (bgfx::isValid(sampler))
        {
            bgfx::destroy(sampler);
        }
        sampler = BGFX_INVALID_HANDLE;
    }
    vertices_ = BGFX_INVALID_HANDLE;
    program_ = BGFX_INVALID_HANDLE;
    color_ = BGFX_INVALID_HANDLE;
    chroma_ = BGFX_INVALID_HANDLE;
}
} // namespace engine::render_detail
