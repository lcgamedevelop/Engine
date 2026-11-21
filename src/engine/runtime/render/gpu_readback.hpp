#pragma once

#include "runtime/performance/function_profile.hpp"

#include "runtime/performance/memory_profiling.hpp"
#include "runtime/performance/profiling.hpp"

#include <bgfx/bgfx.h>

#include <cstdint>
#include <memory>
#include <new>
#include <utility>

namespace engine::render_detail
{
#if defined(__EMSCRIPTEN__)
inline constexpr bool web_readbacks = true;
#else
inline constexpr bool web_readbacks = false;
#endif

enum class GpuReadbackStatus
{
    pending,
    complete,
    failed
};

class GpuReadback final
{
public:
    GpuReadback() = default;
    ~GpuReadback() noexcept
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        reset();
    }
    GpuReadback(const GpuReadback&) = delete;
    GpuReadback& operator=(const GpuReadback&) = delete;

    template <typename Region> [[nodiscard]] bool submit(const Region& region, void* pixels)
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
#if defined(__EMSCRIPTEN__)
        if (web_ || pixels == nullptr)
        {
            return false;
        }
        auto state = std::unique_ptr<WebState>(new (std::nothrow) WebState{});
        if (!state)
        {
            return false;
        }
        state->allocation.track(memory_profile::Pool::frame_readback, state.get(),
                                sizeof(WebState));
        state->request = {pixels, mapped, state.get()};
        if (!bgfx::readAsync(region, state->request))
        {
            return false;
        }
        web_ = std::move(state);
        return true;
#else
        ready_frame_ = bgfx::read(region, pixels);
        return ready_frame_ != UINT32_MAX;
#endif
    }

    [[nodiscard]] GpuReadbackStatus status(std::uint32_t frame) const noexcept
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
#if defined(__EMSCRIPTEN__)
        (void)frame;
        return web_ ? web_->status : GpuReadbackStatus::failed;
#else
        if (ready_frame_ == UINT32_MAX)
        {
            return GpuReadbackStatus::failed;
        }
        return static_cast<std::int32_t>(frame - ready_frame_) < 0 ? GpuReadbackStatus::pending
                                                                   : GpuReadbackStatus::complete;
#endif
    }

    void reset() noexcept
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
#if defined(__EMSCRIPTEN__)
        if (web_ && web_->status == GpuReadbackStatus::pending)
        {
            // The worker callback owns detached state until it releases the staging buffer.
            web_->request.data = nullptr;
            (void)web_.release();
            return;
        }
        web_.reset();
#else
        ready_frame_ = UINT32_MAX;
#endif
    }

private:
#if defined(__EMSCRIPTEN__)
    struct WebState
    {
        bgfx::WebGpuReadback request{};
        GpuReadbackStatus status = GpuReadbackStatus::pending;
        [[no_unique_address]] memory_profile::Allocation allocation;
    };

    static void mapped(void* opaque, bool success) noexcept
    {
        ENGINE_PROFILE_ZONE("Render.webReadbackMapped");
        auto* state = static_cast<WebState*>(opaque);
        if (state->request.data == nullptr)
        {
            delete state;
            return;
        }
        state->status = success ? GpuReadbackStatus::complete : GpuReadbackStatus::failed;
    }

    std::unique_ptr<WebState> web_;
#else
    std::uint32_t ready_frame_ = UINT32_MAX;
#endif
};
} // namespace engine::render_detail
