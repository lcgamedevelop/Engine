#pragma once
#include "runtime/movie/movie_frame.hpp"
#include <array>
#include <bgfx/bgfx.h>
#include <cstdint>
#include <cstddef>

namespace engine::render_detail
{
class FrameOutput;

// Owns uploaded planes for graph execution, including cancellation before submission.
class PreparedMovieFrame final
{
public:
    PreparedMovieFrame() = default;
    ~PreparedMovieFrame() noexcept;
    PreparedMovieFrame(const PreparedMovieFrame&) = delete;
    PreparedMovieFrame& operator=(const PreparedMovieFrame&) = delete;

private:
    friend class MoviePass;
    void release() noexcept;
    std::array<bgfx::TextureHandle, 3> planes_{
        {BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE}};
    std::array<float, 4> params_{};
    std::array<float, 4> chroma_{};
    std::uint16_t width_ = 0;
    std::uint16_t height_ = 0;
};

class MoviePass final
{
public:
    ~MoviePass() noexcept;
    [[nodiscard]] static bool validate(const MovieFrame& frame, const FrameOutput& output,
                                       PreparedMovieFrame& prepared);
    [[nodiscard]] bool upload_plane(const MovieFrame& frame, std::size_t index,
                                    PreparedMovieFrame& prepared);
    void draw(const PreparedMovieFrame& prepared, const FrameOutput& output) const;
    void shutdown() noexcept;

private:
    [[nodiscard]] bool initialize();
    bgfx::ProgramHandle program_ = BGFX_INVALID_HANDLE;
    bgfx::VertexBufferHandle vertices_ = BGFX_INVALID_HANDLE;
    std::array<bgfx::UniformHandle, 3> samplers_{
        {BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE}};
    bgfx::UniformHandle color_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle chroma_ = BGFX_INVALID_HANDLE;
};
} // namespace engine::render_detail
