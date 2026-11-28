#pragma once

#include "runtime/performance/function_profile.hpp"

#include "runtime/performance/memory_profiling.hpp"
#include "runtime/render/gpu_memory_tracker.hpp"
#include "runtime/render/gpu_readback.hpp"

#include <bgfx/bgfx.h>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace engine::render_detail
{
// Final display-space image shared by presentation and ordinary screenshots.
class FrameOutput final
{
public:
    explicit FrameOutput(GpuMemoryTracker* memory) noexcept : memory_(memory)
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
    }
    ~FrameOutput() noexcept;
    FrameOutput(const FrameOutput&) = delete;
    FrameOutput& operator=(const FrameOutput&) = delete;
    [[nodiscard]] bool prepare(std::uint16_t width, std::uint16_t height);
    void clear(std::uint32_t clear_color) const;
    void bind_ui() const;
    void present() const;
    [[nodiscard]] bool capture(SceneScopeId scope, std::uint64_t scene);
    void retire_capture() noexcept;
    [[nodiscard]] GpuReadbackStatus collect(std::uint32_t frame);
    [[nodiscard]] const std::vector<std::byte>& pixels() const noexcept
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        return pixels_;
    }
    [[nodiscard]] std::uint32_t capture_width() const noexcept
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        return capture_width_;
    }
    [[nodiscard]] std::uint32_t capture_height() const noexcept
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        return capture_height_;
    }
    [[nodiscard]] bgfx::TextureHandle color() const noexcept
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        return color_;
    }
    [[nodiscard]] bgfx::FrameBufferHandle target() const noexcept
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        return target_;
    }
    [[nodiscard]] std::uint16_t width() const noexcept
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        return width_;
    }
    [[nodiscard]] std::uint16_t height() const noexcept
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        return height_;
    }
    void release() noexcept;

private:
    void release_target() noexcept;
    void release_readback() noexcept;
    GpuMemoryTracker* memory_;
    [[no_unique_address]] GpuMemoryAllocation color_memory_;
    [[no_unique_address]] GpuMemoryAllocation readback_memory_;
    bgfx::FrameBufferHandle target_ = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle color_ = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle readback_ = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle program_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle sampler_ = BGFX_INVALID_HANDLE;
    bgfx::VertexBufferHandle vertices_ = BGFX_INVALID_HANDLE;
    std::uint16_t width_ = 0, height_ = 0;
    std::uint16_t capture_width_ = 0, capture_height_ = 0;
    bool pending_ = false;
    std::vector<std::byte> pixels_;
    [[no_unique_address]] memory_profile::Allocation pixels_allocation_;
    GpuReadback readback_completion_;
};
} // namespace engine::render_detail
