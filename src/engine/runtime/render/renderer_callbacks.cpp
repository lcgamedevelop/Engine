#include "runtime/render/renderer_callbacks.hpp"

#include "runtime/performance/profiling.hpp"

#if defined(ENGINE_ENABLE_TRACY)
#include <tracy/TracyC.h>
#endif
#include "runtime/task/task_system.hpp"
#include "runtime/utils/path_utils.hpp"

#include <SDL3/SDL.h>
#include <bimg/bimg.h>
#include <bx/readerwriter.h>

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <vector>

namespace engine
{
namespace
{

#if defined(ENGINE_ENABLE_TRACY)
thread_local std::vector<TracyCZoneCtx> bgfx_profile_stack;

void begin_bgfx_profile(const char* name, std::uint32_t color, const char* file, std::uint16_t line)
{
    TracyCZoneCtx context{};
    if (TracyCIsConnected)
    {
        const auto* label = name ? name : "bgfx/unnamed";
        const auto* source = file ? file : "bgfx";
        const auto rgb = ((color & 0xff) << 16) | (color & 0xff00) | ((color >> 16) & 0xff);
        // The allocated source location copies names, including transient view labels.
        const auto location = ___tracy_alloc_srcloc_name(line, source, std::strlen(source), "", 0,
                                                         label, std::strlen(label), rgb);
        context = ___tracy_emit_zone_begin_alloc(location, 1);
    }
    // Keep inactive entries too: connecting during an enclosing scope must not unbalance ends.
    bgfx_profile_stack.push_back(context);
}
#endif

struct ImageWriter final : bx::WriterI
{
    explicit ImageWriter(SDL_IOStream& stream) noexcept : stream_(stream)
    {
    }

    std::int32_t write(const void* data, std::int32_t size, bx::Error* error) override
    {
        if (size < 0)
        {
            error->setError(bx::kErrorReaderWriterWrite, "Invalid image write size");
            return 0;
        }
        const auto written = SDL_WriteIO(&stream_, data, static_cast<std::size_t>(size));
        if (written != static_cast<std::size_t>(size))
        {
            error->setError(bx::kErrorReaderWriterWrite, "Cannot write screenshot");
        }
        return static_cast<std::int32_t>(written);
    }

private:
    SDL_IOStream& stream_;
};

} // namespace

struct RenderSystem::RendererCallbacks::Capture
{
    std::string path;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t pitch = 0;
    bool yflip = false;
    std::vector<std::byte> pixels;

    bool save() const
    {
        ENGINE_PROFILE_ZONE("Render.capture_encode_write");
        const bool png = path.ends_with(".png");
        const std::string output = png ? path : path + ".tga";
        SDL_IOStream* stream = SDL_IOFromFile(output.c_str(), "wb");
        bool success = false;
        if (stream != nullptr)
        {
            ImageWriter writer(*stream);
            bx::Error error;
            const auto written =
                png ? bimg::imageWritePng(&writer, width, height, pitch, pixels.data(),
                                          bimg::TextureFormat::BGRA8, yflip, &error)
                    : bimg::imageWriteTga(&writer, width, height, pitch, pixels.data(), false,
                                          yflip, &error);
            const bool closed = SDL_CloseIO(stream);
            success = written > 0 && error.isOk() && closed;
        }
        if (!success)
        {
            SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Screenshot write failed: %s", output.c_str());
            if (stream != nullptr)
            {
                (void)SDL_RemovePath(output.c_str());
            }
        }
        return success;
    }
};

void RenderSystem::RendererCallbacks::process(TaskSystem* tasks)
{
    if (writing.valid())
    {
        if (writing.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        {
            return;
        }
        status.store(writing.get() ? ScreenshotStatus::complete : ScreenshotStatus::failed);
    }
    std::shared_ptr<Capture> next;
    {
        std::lock_guard lock(mutex);
        next = capture;
    }
    if (next == nullptr)
    {
        return;
    }
    if (tasks != nullptr)
    {
        // 闭包只拥有像素与路径，不借用渲染器；拒绝提交时保留读回槽下帧重试。
        writing = tasks->submit("Render.capture_encode_write", [next] { return next->save(); });
        if (!writing.valid())
        {
            return;
        }
    }
    else
    {
        status.store(next->save() ? ScreenshotStatus::complete : ScreenshotStatus::failed);
    }
    std::lock_guard lock(mutex);
    if (capture == next)
    {
        capture.reset();
    }
}

void RenderSystem::RendererCallbacks::finish() noexcept
{
    // 生命周期边界允许等待；正常帧只在 process() 中非阻塞查询。
    if (writing.valid())
    {
        status.store(writing.get() ? ScreenshotStatus::complete : ScreenshotStatus::failed);
    }
    else if (status.load() == ScreenshotStatus::pending)
    {
        status.store(ScreenshotStatus::failed);
    }
    capture.reset();
}

void RenderSystem::RendererCallbacks::fatal(const char* file, std::uint16_t line,
                                            bgfx::Fatal::Enum code, const char* message)
{
    SDL_LogError(SDL_LOG_CATEGORY_RENDER, "bgfx fatal %s:%u: %s", file, line, message);
    if (code != bgfx::Fatal::DebugCheck)
    {
        // bgfx 明确将非 DebugCheck 定义为不可恢复；普通渲染失败由返回值处理。
        std::abort();
    }
}

void RenderSystem::RendererCallbacks::traceVargs(const char*, std::uint16_t, const char* format,
                                                 va_list arguments)
{
    SDL_LogMessageV(SDL_LOG_CATEGORY_RENDER, SDL_LOG_PRIORITY_DEBUG, format, arguments);
}

void RenderSystem::RendererCallbacks::profilerBegin(const char* name, std::uint32_t color,
                                                    const char* file, std::uint16_t line)
{
#if defined(ENGINE_ENABLE_TRACY)
    begin_bgfx_profile(name, color, file, line);
#else
    (void)name;
    (void)color;
    (void)file;
    (void)line;
#endif
}

void RenderSystem::RendererCallbacks::profilerBeginLiteral(const char* name, std::uint32_t color,
                                                           const char* file, std::uint16_t line)
{
    profilerBegin(name, color, file, line);
}

void RenderSystem::RendererCallbacks::profilerEnd()
{
#if defined(ENGINE_ENABLE_TRACY)
    if (bgfx_profile_stack.empty())
    {
        constexpr char message[] = "Bgfx.profilerStackImbalance";
        ENGINE_PROFILE_MESSAGE(message, sizeof(message) - 1);
        return;
    }
    ___tracy_emit_zone_end(bgfx_profile_stack.back());
    bgfx_profile_stack.pop_back();
#endif
}

std::uint32_t RenderSystem::RendererCallbacks::cacheReadSize(std::uint64_t)
{
    return 0;
}

bool RenderSystem::RendererCallbacks::cacheRead(std::uint64_t, void*, std::uint32_t)
{
    return false;
}

void RenderSystem::RendererCallbacks::cacheWrite(std::uint64_t, const void*, std::uint32_t)
{
}

void RenderSystem::RendererCallbacks::captureBegin(std::uint32_t, std::uint32_t, std::uint32_t,
                                                   bgfx::TextureFormat::Enum, bool)
{
}

void RenderSystem::RendererCallbacks::captureEnd()
{
}

void RenderSystem::RendererCallbacks::captureFrame(const void*, std::uint32_t)
{
}

void RenderSystem::RendererCallbacks::screenShot(const char* path, std::uint32_t width,
                                                 std::uint32_t height, std::uint32_t pitch,
                                                 bgfx::TextureFormat::Enum format, const void* data,
                                                 std::uint32_t size, bool yflip)
{
    ENGINE_PROFILE_ZONE("Render.capture_copy");
    const std::string requested = path != nullptr ? path : "";
    std::lock_guard lock(mutex);
    if (status.load() == ScreenshotStatus::pending &&
        (pending_path != requested || readback_received))
    {
        return;
    }
    pending_path = requested;
    readback_received = true;
    const std::uint64_t bytes = static_cast<std::uint64_t>(pitch) * height;
    constexpr std::uint64_t maximum_bytes = 256ULL * 1024 * 1024;
    if (requested.empty() || format != bgfx::TextureFormat::BGRA8 || data == nullptr ||
        width == 0 || height == 0 || static_cast<std::uint64_t>(width) * 4 > pitch ||
        bytes > size || bytes > maximum_bytes)
    {
        status.store(ScreenshotStatus::failed);
        return;
    }
    auto next = std::make_shared<Capture>();
    next->path = requested;
    next->width = width;
    next->height = height;
    next->pitch = pitch;
    next->yflip = yflip;
    const auto* begin = static_cast<const std::byte*>(data);
    next->pixels.assign(begin, begin + static_cast<std::size_t>(bytes));
    capture = std::move(next);
    status.store(ScreenshotStatus::pending);
}

} // namespace engine
