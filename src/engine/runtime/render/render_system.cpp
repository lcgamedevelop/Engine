#include "runtime/render/render_system.hpp"
#include "runtime/render/rvt/rvt_debug_view.hpp"
#include "runtime/render/rvt/rvt_pass.hpp"
#include "runtime/render/vsm/virtual_shadow_cache.hpp"
#include "runtime/render/vsm/virtual_shadow_pass.hpp"
#include "runtime/render/vsm/vsm_debug_view.hpp"

#include "runtime/render/frame_counters.hpp"
#include "runtime/render/gpu_memory_tracker.hpp"
#include "runtime/render/passes/frame_output.hpp"
#include "runtime/render/passes/meshlet_cull_pass.hpp"

#include "runtime/font/font_system.hpp"
#include "runtime/performance/memory_profiling.hpp"
#include "runtime/performance/function_profile.hpp"
#include "runtime/performance/profiling.hpp"
#include "runtime/render/movie_pass.hpp"
#include "runtime/render/render_constants.hpp"
#include "runtime/render/render_extension.hpp"
#include "runtime/render/render_graph.hpp"
#include "runtime/render/render_sampling.hpp"
#include "runtime/render/render_surface.hpp"
#include "runtime/render/render_view_order.hpp"
#include "runtime/render/renderer_callbacks.hpp"
#include "runtime/render/scene_gpu_resources.hpp"
#include "runtime/task/task_system.hpp"
#include "runtime/ui/ui_system.hpp"
#include "runtime/utils/path_utils.hpp"
#include "runtime/window/window_system.hpp"
#include "runtime/world/world_system.hpp"

#include <SDL3/SDL.h>
#include <bx/allocator.h>

#include <algorithm>
#if defined(ENGINE_ENABLE_TEST_SUPPORT)
#include <atomic>
#endif
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <new>
#include <string>
#include <system_error>

namespace engine
{

// SDL 窗口和 bgfx 生命周期都由主线程驱动；bgfx 只支持一个活动实例。
namespace
{
RenderSystem* active_renderer = nullptr;

#if defined(ENGINE_ENABLE_TEST_SUPPORT)
constexpr char legacy_hdr_hint[] = "ENGINE_TEST_LEGACY_HDR";
std::atomic_bool legacy_hdr_requested{false};

void SDLCALL update_hdr_scheduler(void*, const char*, const char*, const char* value)
{
    // The SDL callback may run on the thread setting the hint. Only the render thread
    // accesses the graph. Match SDL_GetHintBoolean(value, false) for the supplied value.
    const bool legacy = value && *value && *value != '0' && SDL_strcasecmp(value, "false") != 0;
    legacy_hdr_requested.store(legacy, std::memory_order_relaxed);
}
#endif

#if defined(ENGINE_ENABLE_MEMORY_PROFILING)
class BgfxCpuAllocator final : public bx::AllocatorI
{
public:
    void* realloc(void* address, std::size_t bytes, std::size_t alignment, const char*,
                  std::uint32_t) noexcept override
    {
        ENGINE_PROFILE_FUNCTION();
        return memory_profile::BackingAllocation::reallocate_block(memory_profile::Pool::bgfx_cpu,
                                                                   address, bytes, alignment);
    }
};
#endif

#if defined(ENGINE_ENABLE_TRACY)
std::size_t profile_view_group(bgfx::ViewId id)
{
    ENGINE_PROFILE_FUNCTION();
    using namespace render_detail;
    if (id >= 3 && id <= 72)
    {
        return 0;
    }
    if ((id >= shadow_view && id < output_clear_view) ||
        std::find(vsm_page_views.begin(), vsm_page_views.end(), id) != vsm_page_views.end())
    {
        return 1;
    }
    if (id == ambient_occlusion_depth_view || id == opaque_depth_view)
    {
        return 2;
    }
    if (id == surface_view)
    {
        return 3;
    }
    if (id == main_view)
    {
        return 4;
    }
    if (id == ambient_occlusion_compute_view)
    {
        return 5;
    }
    if (id >= ssr_hiz_view && id <= ssr_composite_view)
    {
        return 6;
    }
    if (id == transparent_view)
    {
        return 7;
    }
    if (id >= hdr_bright_view && id <= hdr_resolve_view)
    {
        return 8;
    }
    if (id == output_present_view)
    {
        return 9;
    }
    if (id >= meshlet_index_upload_view && id <= meshlet_index_compact_view)
    {
        return 10;
    }
    if (id >= rvt_page_view - 1 && id < rvt_page_view + maximum_rvt_views)
    {
        return 11;
    }
    if (id == vsm_feedback_clear_view || id == vsm_feedback_view)
    {
        return 12;
    }
    if (id == atmosphere_transmittance_view || id == atmosphere_multiscatter_view ||
        id == atmosphere_main_skyview || id == atmosphere_capture_skyview)
    {
        return 14;
    }
    if (id == scene_copy_view)
    {
        return 15;
    }
    if (id == skinning_view)
    {
        return 16;
    }
    return 13;
}

void profile_view_record(const char* kind, std::uint16_t id, const char* name, std::int64_t frame,
                         std::int64_t begin, std::int64_t end, std::int64_t frequency)
{
    ENGINE_PROFILE_FUNCTION();
    std::string escaped;
    for (const unsigned char value : std::string_view(name))
    {
        if (value == '"' || value == '\\')
        {
            escaped.push_back('\\');
        }
        if (value < 0x20)
        {
            constexpr char hex[] = "0123456789abcdef";
            escaped += "\\u00";
            escaped.push_back(hex[value >> 4]);
            escaped.push_back(hex[value & 15]);
        }
        else
        {
            escaped.push_back(static_cast<char>(value));
        }
    }
    std::array<char, 2048> message{};
    const auto count =
        std::snprintf(message.data(), message.size(),
                      "%s {\"viewId\":%u,\"name\":\"%s\",\"group\":%zu,\"sourceFrame\":%lld,"
                      "\"begin\":%lld,\"end\":%lld,\"frequency\":%lld}",
                      kind, static_cast<unsigned int>(id), escaped.c_str(), profile_view_group(id),
                      static_cast<long long>(frame), static_cast<long long>(begin),
                      static_cast<long long>(end), static_cast<long long>(frequency));
    if (count > 0 && static_cast<std::size_t>(count) < message.size())
    {
        ENGINE_PROFILE_MESSAGE(message.data(), static_cast<std::size_t>(count));
    }
}

void profile_renderer_stats(const RvtStatus& rvt, const VsmStatus& vsm)
{
    ENGINE_PROFILE_ZONE("Render.profileStats");
    if (!ENGINE_PROFILE_CONNECTED())
    {
        return;
    }
    const auto* stats = bgfx::getStats();
    if (stats == nullptr)
    {
        return;
    }
    if (rvt.effective)
    {
        ENGINE_PROFILE_PLOT("Rvt.requiredPages", static_cast<std::int64_t>(rvt.required_pages));
        ENGINE_PROFILE_PLOT("Rvt.readyPages", static_cast<std::int64_t>(rvt.ready_pages));
        ENGINE_PROFILE_PLOT("Rvt.residentBytes", static_cast<std::int64_t>(rvt.resident_bytes));
    }
    if (vsm.state != VsmState::disabled)
    {
        ENGINE_PROFILE_PLOT("Vsm.demandedPages", static_cast<std::int64_t>(vsm.demanded_pages));
        ENGINE_PROFILE_PLOT("Vsm.redrawnPages", static_cast<std::int64_t>(vsm.redrawn_pages));
        ENGINE_PROFILE_PLOT("Vsm.fallbackPages", static_cast<std::int64_t>(vsm.fallback_pages));
        ENGINE_PROFILE_PLOT("Vsm.residentBytes", static_cast<std::int64_t>(vsm.resident_bytes));
    }
    // Stats 对应已经完成的后端帧，可能滞后于提交线程。
    // cpuTimeFrame 包含玩法与限帧等待，不能作为渲染 CPU 耗时。
    if (stats->cpuTimerFreq > 0)
    {
        const double milliseconds = 1000.0 / static_cast<double>(stats->cpuTimerFreq);
        if (stats->cpuTimeFrame > 0)
        {
            ENGINE_PROFILE_PLOT("Bgfx.frameIntervalMs", stats->cpuTimeFrame * milliseconds);
        }
        if (stats->cpuTimeBegin > 0 && stats->cpuTimeEnd >= stats->cpuTimeBegin)
        {
            ENGINE_PROFILE_PLOT("Bgfx.renderThreadSubmitMs",
                                (stats->cpuTimeEnd - stats->cpuTimeBegin) * milliseconds);
        }
        if (stats->waitRender >= 0)
        {
            ENGINE_PROFILE_PLOT("Bgfx.waitRenderMs", stats->waitRender * milliseconds);
        }
        if (stats->waitSubmit >= 0)
        {
            ENGINE_PROFILE_PLOT("Bgfx.waitSubmitMs", stats->waitSubmit * milliseconds);
        }
    }
    // GPU 时间戳使用独立时钟；不支持或不可用的数据不生成采样值。
    if (stats->gpuTimerFreq > 0 && stats->gpuTimeBegin > 0 &&
        stats->gpuTimeEnd >= stats->gpuTimeBegin)
    {
        static thread_local std::int64_t last_gpu_begin = 0;
        const double milliseconds = 1000.0 / static_cast<double>(stats->gpuTimerFreq);
        ENGINE_PROFILE_PLOT("Bgfx.gpuFrameMs",
                            (stats->gpuTimeEnd - stats->gpuTimeBegin) * milliseconds);
        ENGINE_PROFILE_PLOT("Bgfx.gpuFrameNumber", static_cast<std::int64_t>(stats->gpuFrameNum));
        if (last_gpu_begin != stats->gpuTimeBegin)
        {
            last_gpu_begin = stats->gpuTimeBegin;
            profile_view_record("Render.gpuFrame", UINT16_MAX, "GPU command buffer",
                                stats->gpuFrameNum, stats->gpuTimeBegin, stats->gpuTimeEnd,
                                stats->gpuTimerFreq);
        }
    }
    constexpr std::array cpu_names{
        "Bgfx.cpuPlanarMs",      "Bgfx.cpuShadowMs",      "Bgfx.cpuDepthMs",
        "Bgfx.cpuSurfaceMs",     "Bgfx.cpuOpaqueMs",      "Bgfx.cpuAoMs",
        "Bgfx.cpuSsrMs",         "Bgfx.cpuTransparentMs", "Bgfx.cpuPostMs",
        "Bgfx.cpuPresentMs",     "Bgfx.cpuMeshletMs",     "Bgfx.cpuRvtMs",
        "Bgfx.cpuVsmFeedbackMs", "Bgfx.cpuOtherMs",       "Bgfx.cpuAtmosphereMs",
        "Bgfx.cpuSceneCopyMs",   "Bgfx.cpuSkinningMs"};
    constexpr std::array gpu_names{
        "Bgfx.gpuPlanarMs",      "Bgfx.gpuShadowMs",      "Bgfx.gpuDepthMs",
        "Bgfx.gpuSurfaceMs",     "Bgfx.gpuOpaqueMs",      "Bgfx.gpuAoMs",
        "Bgfx.gpuSsrMs",         "Bgfx.gpuTransparentMs", "Bgfx.gpuPostMs",
        "Bgfx.gpuPresentMs",     "Bgfx.gpuMeshletMs",     "Bgfx.gpuRvtMs",
        "Bgfx.gpuVsmFeedbackMs", "Bgfx.gpuOtherMs",       "Bgfx.gpuAtmosphereMs",
        "Bgfx.gpuSceneCopyMs",   "Bgfx.gpuSkinningMs"};
    std::array<double, cpu_names.size()> cpu{}, gpu{};
    std::array<bool, cpu_names.size()> cpu_valid{}, gpu_valid{};
    static thread_local std::array<std::int64_t, 256> cpu_begin{}, gpu_begin{};
    static thread_local std::array<std::uint32_t, 256> gpu_frame{};
    for (std::uint16_t index = 0; stats->viewStats && index < stats->numViews; ++index)
    {
        const auto& view = stats->viewStats[index];
        if (view.view >= cpu_begin.size())
        {
            continue;
        }
        const auto group = profile_view_group(view.view);
        if (stats->cpuTimerFreq > 0 && view.cpuTimeBegin > 0 &&
            view.cpuTimeEnd >= view.cpuTimeBegin)
        {
            cpu[group] += (view.cpuTimeEnd - view.cpuTimeBegin) *
                          (1000.0 / static_cast<double>(stats->cpuTimerFreq));
            cpu_valid[group] = true;
            if (cpu_begin[view.view] != view.cpuTimeBegin)
            {
                cpu_begin[view.view] = view.cpuTimeBegin;
                profile_view_record("Render.cpuView", view.view, view.name, -1, view.cpuTimeBegin,
                                    view.cpuTimeEnd, stats->cpuTimerFreq);
            }
        }
        if (stats->gpuTimerFreq > 0 && view.gpuTimeBegin > 0 &&
            view.gpuTimeEnd >= view.gpuTimeBegin &&
            (gpu_begin[view.view] != view.gpuTimeBegin || gpu_frame[view.view] != view.gpuFrameNum))
        {
            gpu_begin[view.view] = view.gpuTimeBegin;
            gpu_frame[view.view] = view.gpuFrameNum;
            profile_view_record("Render.gpuView", view.view, view.name, view.gpuFrameNum,
                                view.gpuTimeBegin, view.gpuTimeEnd, stats->gpuTimerFreq);
            // Grouped plots still describe one completed GPU frame only; raw records retain
            // independently delayed views for the offline per-source-frame analysis.
            if (view.gpuFrameNum == stats->gpuFrameNum)
            {
                gpu[group] += (view.gpuTimeEnd - view.gpuTimeBegin) *
                              (1000.0 / static_cast<double>(stats->gpuTimerFreq));
                gpu_valid[group] = true;
            }
        }
    }
    for (std::size_t group = 0; group < cpu.size(); ++group)
    {
        if (cpu_valid[group])
        {
            ENGINE_PROFILE_PLOT(cpu_names[group], cpu[group]);
        }
        if (gpu_valid[group])
        {
            ENGINE_PROFILE_PLOT(gpu_names[group], gpu[group]);
        }
    }
    ENGINE_PROFILE_PLOT("Bgfx.drawCalls", static_cast<std::int64_t>(stats->numDraw));
    ENGINE_PROFILE_PLOT("Bgfx.computeCalls", static_cast<std::int64_t>(stats->numCompute));
    ENGINE_PROFILE_PLOT("Bgfx.blitCalls", static_cast<std::int64_t>(stats->numBlit));
    ENGINE_PROFILE_PLOT("Bgfx.triangles",
                        static_cast<std::int64_t>(stats->numPrims[bgfx::Topology::TriList]));
    ENGINE_PROFILE_PLOT("Bgfx.textures", static_cast<std::int64_t>(stats->numTextures));
    ENGINE_PROFILE_PLOT("Bgfx.vertexBuffers", static_cast<std::int64_t>(stats->numVertexBuffers));
    ENGINE_PROFILE_PLOT("Bgfx.indexBuffers", static_cast<std::int64_t>(stats->numIndexBuffers));
    ENGINE_PROFILE_PLOT("Bgfx.frameBuffers", static_cast<std::int64_t>(stats->numFrameBuffers));
    // 资源估算与驱动上报占用口径不同且有重叠，分别记录，不能相加。
    if (stats->textureMemoryUsed >= 0)
    {
        ENGINE_PROFILE_PLOT("Bgfx.textureEstimatedBytes", stats->textureMemoryUsed);
    }
    if (stats->rtMemoryUsed >= 0)
    {
        ENGINE_PROFILE_PLOT("Bgfx.renderTargetEstimatedBytes", stats->rtMemoryUsed);
    }
    if (stats->transientVbUsed >= 0)
    {
        ENGINE_PROFILE_PLOT("Bgfx.transientVertexBytes",
                            static_cast<std::int64_t>(stats->transientVbUsed));
    }
    if (stats->transientIbUsed >= 0)
    {
        ENGINE_PROFILE_PLOT("Bgfx.transientIndexBytes",
                            static_cast<std::int64_t>(stats->transientIbUsed));
    }
    if (stats->gpuMemoryUsed >= 0)
    {
        ENGINE_PROFILE_PLOT("Bgfx.gpuResidentBytes", stats->gpuMemoryUsed);
    }
    if (stats->gpuMemoryMax >= 0)
    {
        ENGINE_PROFILE_PLOT("Bgfx.gpuBudgetBytes", stats->gpuMemoryMax);
    }
}
#endif
} // namespace

RenderSystem::RenderSystem(WindowSystem& windows, WorldSystem* world, FontSystem* fonts,
                           UiSystem* ui, TaskSystem* tasks, const RenderConfig& config) noexcept
    : windows_(windows), world_(world), fonts_(fonts), ui_(ui), tasks_(tasks), config_(config)
{
    ENGINE_PROFILE_FUNCTION();
}

RenderSystem::~RenderSystem() noexcept
{
    ENGINE_PROFILE_FUNCTION();
    shutdown();
    for (auto* extension : extensions_)
    {
        extension->renderer_ = nullptr;
    }
    extensions_.clear();
}

bool RenderSystem::bind_world(WorldSystem* world, FontSystem* fonts, UiSystem* ui)
{
    ENGINE_PROFILE_FUNCTION();
    if (!is_initialized() || invoking_extensions_ ||
        ((world == nullptr || fonts == nullptr || ui == nullptr) &&
         (world != nullptr || fonts != nullptr || ui != nullptr)) ||
        (world != nullptr &&
         (!world->is_initialized() || !fonts->is_initialized() || !ui->is_initialized())))
    {
        return false;
    }
    if (world_ != world || fonts_ != fonts || ui_ != ui)
    {
        release_scene();
        world_ = world;
        fonts_ = fonts;
        ui_ = ui;
    }
    return true;
}

bool RenderSystem::add_extension(RenderExtension& extension)
{
    ENGINE_PROFILE_FUNCTION();
    if (invoking_extensions_ || extension.renderer_ != nullptr)
    {
        return false;
    }
    extensions_.push_back(&extension);
    extension.renderer_ = this;
    if (preparation_ && preparation_->status == RenderPrepareStatus::ready)
    {
        preparation_->status = RenderPrepareStatus::preparing;
    }
    return true;
}

bool RenderSystem::remove_extension(RenderExtension& extension) noexcept
{
    ENGINE_PROFILE_FUNCTION();
    if (invoking_extensions_ || extension.renderer_ != this)
    {
        return false;
    }
    detach_extension(extension);
    invoking_extensions_ = true;
    extension.release_scene();
    extension.release_gpu();
    invoking_extensions_ = false;
    return true;
}

void RenderSystem::detach_extension(RenderExtension& extension) noexcept
{
    ENGINE_PROFILE_FUNCTION();
    std::erase(extensions_, &extension);
    extension.renderer_ = nullptr;
    if (preparation_ && preparation_->status == RenderPrepareStatus::ready)
    {
        preparation_->status = RenderPrepareStatus::preparing;
    }
}

void RenderSystem::release_extensions_scene() noexcept
{
    ENGINE_PROFILE_FUNCTION();
    published_extension_scope_ = {};
    invoking_extensions_ = true;
    for (auto* extension : extensions_)
    {
        extension->release_scene();
    }
    invoking_extensions_ = false;
}

bool RenderSystem::on_initialize(const EngineConfig&)
{
    ENGINE_PROFILE_FUNCTION();
    // 初始化前的非法准备请求不能把新的渲染生命周期永久留在 failed。
    preparation_.reset();
    if (config_.cluster_tile_size < 16 || config_.cluster_tile_size > 256 ||
        config_.cluster_depth_slices == 0 || config_.cluster_depth_slices > 64 ||
        config_.maximum_meshlet_candidates == 0 ||
        config_.maximum_meshlet_candidates > UINT32_MAX / 128 ||
        config_.maximum_meshlet_candidates > UINT32_MAX / 124 || config_.maximum_lights == 0 ||
        config_.maximum_lights > 4096 || config_.maximum_cluster_lights == 0 ||
        config_.maximum_cluster_lights > 256 || config_.shadow_atlas_size < 512 ||
        config_.shadow_atlas_size > 8192 ||
        (config_.shadow_atlas_size & (config_.shadow_atlas_size - 1)) != 0 ||
        config_.local_shadow_tile_size < 64 || config_.local_shadow_tile_size > 2048 ||
        (config_.local_shadow_tile_size & (config_.local_shadow_tile_size - 1)) != 0 ||
        config_.local_shadow_tile_size > config_.shadow_atlas_size ||
        config_.vsm_physical_pages == 0 ||
        config_.vsm_physical_pages > render_detail::vsm_max_physical_pages ||
        config_.vsm_pages_per_frame == 0 || config_.vsm_pages_per_frame > 16 ||
        config_.maximum_local_shadow_faces > 24 || config_.maximum_planar_reflections > 2 ||
        !render_detail::valid_msaa_samples(config_.msaa_samples))
    {
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Renderer lighting/reflection budgets are invalid");
        return false;
    }
    if (!windows_.is_initialized() || windows_.native_window() == nullptr ||
        (tasks_ != nullptr && !tasks_->is_initialized()) || config_.texture_batch_size == 0 ||
        config_.texture_batch_size > 64 || config_.texture_staging_bytes == 0 ||
        config_.prepare_items_per_step == 0 || config_.upload_bytes_per_step == 0 ||
        !std::isfinite(config_.prepare_seconds_per_step) || config_.prepare_seconds_per_step <= 0)
    {
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Renderer requires a ready window");
        return false;
    }
    surface_.reset(new (std::nothrow) render_detail::RenderSurface{});
    if (surface_ == nullptr ||
        !surface_->initialize(*windows_.native_window(), this, suspend_renderer))
    {
        return false;
    }
    return initialize_renderer();
}

void RenderSystem::suspend_renderer(void* context) noexcept
{
    ENGINE_PROFILE_FUNCTION();
    static_cast<RenderSystem*>(context)->shutdown_renderer(false);
}

bool RenderSystem::initialize_renderer()
{
    ENGINE_PROFILE_FUNCTION();
    if (active_renderer != nullptr || !windows_.is_initialized() ||
        windows_.native_window() == nullptr)
    {
        SDL_LogError(SDL_LOG_CATEGORY_RENDER,
                     "Renderer requires a ready window and no active bgfx");
        return false;
    }

    bgfx::Init init{};
    bool available = false;
    if (surface_ == nullptr || !surface_->prepare(init, available))
    {
        return false;
    }
    if (!available)
    {
        // 平台表面暂不可用时保留系统；调用方恢复提交后再建立GPU设备。
        return true;
    }
    width_ = init.swapChain.width;
    height_ = init.swapChain.height;
    init.fallback = false;
    init.reset = render_detail::reset_flags |
                 (vsync_enabled_ ? BGFX_RESET_VSYNC : BGFX_RESET_NONE) |
                 (config_.max_anisotropy ? BGFX_RESET_MAXANISOTROPY : BGFX_RESET_NONE);
    if (config_.transient_vertex_bytes != 0)
    {
        init.limits.maxTransientVbSize = config_.transient_vertex_bytes;
    }
    if (config_.transient_index_bytes != 0)
    {
        init.limits.maxTransientIbSize = config_.transient_index_bytes;
    }
    if (callbacks_ == nullptr)
    {
        callbacks_.reset(new (std::nothrow) RendererCallbacks{});
        if (callbacks_ == nullptr)
        {
            return false;
        }
    }
    init.callback = callbacks_.get();
#if defined(ENGINE_ENABLE_MEMORY_PROFILING)
    static BgfxCpuAllocator cpu_allocator;
    init.allocator = &cpu_allocator;
#endif
    if (!bgfx::init(init))
    {
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "bgfx initialization failed");
        return false;
    }
    renderer_initialized_ = true;
    ++device_generation_;
#if defined(ENGINE_ENABLE_TEST_SUPPORT)
    if (!SDL_AddHintCallback(legacy_hdr_hint, update_hdr_scheduler, nullptr))
    {
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Cannot watch HDR scheduler hint: %s", SDL_GetError());
        return false;
    }
    const bool legacy_hdr = legacy_hdr_requested.load(std::memory_order_relaxed);
    SDL_Log("HDR scheduler: %s", legacy_hdr ? "legacy" : "render graph");
    if (!legacy_hdr)
#endif
    {
        render_graph_ = std::make_unique<render_detail::RenderGraph>();
    }
#if defined(ENGINE_ENABLE_MEMORY_PROFILING)
    const bool threaded = (bgfx::getCaps()->supported & BGFX_CAPS_RENDERER_MULTITHREADED) != 0;
    gpu_memory_ = std::make_unique<render_detail::GpuMemoryTracker>(device_generation_, threaded);
#endif
    vsync_applied_ = vsync_enabled_;
    max_anisotropy_applied_ = config_.max_anisotropy;
    active_renderer = this;
    if (bgfx::getRendererType() != init.type)
    {
        SDL_LogError(
            SDL_LOG_CATEGORY_RENDER, "Required renderer %s is unavailable; bgfx selected %s",
            bgfx::getRendererName(init.type), bgfx::getRendererName(bgfx::getRendererType()));
        return false;
    }
    if (!supports_msaa_samples(config_.msaa_samples) ||
        (config_.max_anisotropy && !supports_max_anisotropy()))
    {
        SDL_LogError(SDL_LOG_CATEGORY_RENDER,
                     "Requested scene sampling is unsupported (MSAA=%u anisotropy=%d)",
                     config_.msaa_samples, config_.max_anisotropy);
        return false;
    }

    if (!frame_output_)
    {
        frame_output_ = std::make_unique<render_detail::FrameOutput>(gpu_memory_.get());
    }
    if (!frame_output_->prepare(static_cast<std::uint16_t>(width_),
                                static_cast<std::uint16_t>(height_)))
    {
        return false;
    }
    bgfx::setViewName(render_detail::main_view, "Main window");
    constexpr std::array shadow_names = {"Directional shadow 1", "Directional shadow 2",
                                         "Directional shadow 3", "Directional shadow 4"};
    for (std::uint16_t index = 0; index < render_detail::maximum_shadow_cascades; ++index)
    {
        bgfx::setViewName(render_detail::shadow_view + index, shadow_names[index]);
    }
    bgfx::setViewClear(render_detail::main_view, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH,
                       idle_clear_color_, 1.0F, 0);
    apply_debug_state();
    SDL_Log("Renderer initialized: %s (%u x %u)", bgfx::getRendererName(bgfx::getRendererType()),
            width_, height_);
    return true;
}

bool RenderSystem::gpu_ready() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return renderer_initialized_;
}

std::uint32_t RenderSystem::frame_number() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return frame_number_;
}

bool RenderSystem::viewport_size(Vec2& output) const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    output = {};
    if (!is_initialized() || !renderer_initialized_ || width_ == 0 || height_ == 0)
    {
        return false;
    }
    output = {static_cast<double>(width_), static_cast<double>(height_)};
    return true;
}

void RenderSystem::apply_debug_state()
{
    ENGINE_PROFILE_FUNCTION();
    std::uint32_t flags = stats_visible() ? BGFX_DEBUG_STATS : BGFX_DEBUG_NONE;
#if defined(ENGINE_ENABLE_TRACY)
    // Stage-boundary sampling changes native pass scheduling; keep it scoped to a capture.
    if (ENGINE_PROFILE_CONNECTED())
    {
        flags |= BGFX_DEBUG_PROFILER;
    }
#endif
    bgfx::setDebug(flags);
}

void RenderSystem::set_stats_visible(bool visible)
{
    ENGINE_PROFILE_FUNCTION();
    (void)set_debug_feature(RenderFeature::stats, visible);
}

bool RenderSystem::set_vsync(bool enabled)
{
    ENGINE_PROFILE_FUNCTION();
    if (!is_initialized())
    {
        return false;
    }
    vsync_enabled_ = enabled;
    return true;
}

bool RenderSystem::vsync() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return vsync_enabled_;
}

bool RenderSystem::set_msaa_samples(std::uint8_t samples)
{
    ENGINE_PROFILE_FUNCTION();
    if (!is_initialized() || !supports_msaa_samples(samples))
    {
        return false;
    }
    config_.msaa_samples = samples;
    return true;
}

std::uint8_t RenderSystem::msaa_samples() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return config_.msaa_samples;
}

bool RenderSystem::supports_msaa_samples(std::uint8_t samples) const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    if (!renderer_initialized_ || !render_detail::valid_msaa_samples(samples))
    {
        return false;
    }
    const auto* caps = bgfx::getCaps();
    if (caps == nullptr)
    {
        return false;
    }
    if (samples == 1)
    {
        return true;
    }
    const auto required = render_detail::render_target_sample_capability(samples);
    for (const auto format : {bgfx::TextureFormat::RGBA16F, bgfx::TextureFormat::D32F})
    {
        if ((caps->formats[format] & required) == 0)
        {
            return false;
        }
    }
    return true;
}

bool RenderSystem::set_max_anisotropy(bool enabled)
{
    ENGINE_PROFILE_FUNCTION();
    if (!is_initialized() || !renderer_initialized_ || (enabled && !supports_max_anisotropy()))
    {
        return false;
    }
    config_.max_anisotropy = enabled;
    return true;
}

bool RenderSystem::max_anisotropy() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return config_.max_anisotropy;
}

bool RenderSystem::supports_max_anisotropy() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    const auto* caps = renderer_initialized_ ? bgfx::getCaps() : nullptr;
    return caps != nullptr && caps->maxAnisotropy > 1;
}

bool RenderSystem::stats_visible() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return debug_requests_[static_cast<std::size_t>(RenderFeature::stats)];
}

render_detail::FramePrepareStatus RenderSystem::prepare_frame()
{
    using Status = render_detail::FramePrepareStatus;
    ENGINE_PROFILE_ZONE("Render.prepareFrame");
    if (callbacks_ != nullptr)
    {
        callbacks_->process(tasks_);
    }
    if (!is_initialized())
    {
        return Status::failed;
    }
    if (!windows_.can_render())
    {
        return Status::deferred;
    }

    if (!renderer_initialized_)
    {
        if (!initialize_renderer())
        {
            return Status::failed;
        }
        if (!renderer_initialized_)
        {
            return Status::deferred;
        }
    }

    // Collectors can connect or disconnect while the same renderer stays alive.
#if defined(ENGINE_ENABLE_TRACY)
    apply_debug_state();
#endif
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bgfx::SwapChain swap_chain{};
    if (surface_ == nullptr)
    {
        return Status::failed;
    }
    const auto change = surface_->poll(swap_chain, width, height);
    if (change == render_detail::SurfaceChange::error)
    {
        return Status::failed;
    }
    const bool surface_changed = change != render_detail::SurfaceChange::unchanged;
    if (change == render_detail::SurfaceChange::recreate)
    {
        shutdown_renderer();
        if (!initialize_renderer())
        {
            return Status::failed;
        }
        if (!renderer_initialized_)
        {
            return Status::deferred;
        }
    }
    if (surface_changed || width != width_ || height != height_ ||
        vsync_enabled_ != vsync_applied_ || config_.max_anisotropy != max_anisotropy_applied_)
    {
        if (surface_changed || width != width_ || height != height_)
        {
            cancel_hdr_capture();
        }
        bgfx::reset(render_detail::reset_flags |
                        (vsync_enabled_ ? BGFX_RESET_VSYNC : BGFX_RESET_NONE) |
                        (config_.max_anisotropy ? BGFX_RESET_MAXANISOTROPY : BGFX_RESET_NONE),
                    &swap_chain);
        vsync_applied_ = vsync_enabled_;
        max_anisotropy_applied_ = config_.max_anisotropy;
        width_ = width;
        height_ = height;
    }

    if (!frame_output_)
    {
        frame_output_ = std::make_unique<render_detail::FrameOutput>(gpu_memory_.get());
    }
    if (!frame_output_->prepare(static_cast<std::uint16_t>(width_),
                                static_cast<std::uint16_t>(height_)))
    {
        return Status::failed;
    }
    advance_screenshots();
    return Status::ready;
}

RenderSubmitStatus RenderSystem::render(Camera camera)
{
    render_detail::ui_draws = 0;
    scene_collected_ = false;
    rvt_debug_visible_ = false;
    vsm_debug_visible_ = false;
    rvt_status_.effective = false;
    if (rvt_status_.requested)
    {
        rvt_status_.reason = "No drawable scene view";
    }
    if (vsm_pass_)
    {
        vsm_pass_->poll(frame_number_);
    }
    ENGINE_PROFILE_ZONE("Render.frame");
    if (camera.instance_id() != 0 && !camera)
    {
        return RenderSubmitStatus::failed;
    }
    const auto prepared = prepare_frame();
    if (prepared != render_detail::FramePrepareStatus::ready)
    {
        return prepared == render_detail::FramePrepareStatus::deferred
                   ? RenderSubmitStatus::deferred
                   : RenderSubmitStatus::failed;
    }

    RenderSettings current_settings;
    {
        ENGINE_PROFILE_ZONE("Render.resolveSettings");
        if (!resolve_settings(current_settings))
        {
            return RenderSubmitStatus::failed;
        }
    }
    bgfx::setViewOrder(0, render_detail::scene_view_order.size(),
                       render_detail::scene_view_order.data());
    if (!world_ || !world_->scene())
    {
        release_vsm();
    }
    if (!world_ || !world_->scene() || !current_settings.atmosphere.rvt ||
        !current_settings.atmosphere.rvt->enabled)
    {
        release_rvt();
    }
    bgfx::setViewFrameBuffer(render_detail::main_view, BGFX_INVALID_HANDLE);
    bgfx::setViewClear(render_detail::main_view, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH,
                       current_settings.clear_color, 1.0F, 0);
    bgfx::setViewRect(render_detail::main_view, 0, 0, static_cast<std::uint16_t>(width_),
                      static_cast<std::uint16_t>(height_));
    bgfx::touch(render_detail::main_view);
    if (scene_resources_ != nullptr)
    {
        scene_resources_->view_stats.clear();
        scene_resources_->culling_applied = false;
        scene_resources_->lod_applied = false;
        scene_resources_->debug_draw.reset_frame();
    }
    auto* hdr_scheduler = render_graph_.get();
#if defined(ENGINE_ENABLE_TEST_SUPPORT)
    // One selection for the entire frame; a concurrent hint change takes effect next frame.
    if (legacy_hdr_requested.load(std::memory_order_relaxed))
    {
        hdr_scheduler = nullptr;
    }
    else if (!hdr_scheduler)
    {
        render_graph_ = std::make_unique<render_detail::RenderGraph>();
        hdr_scheduler = render_graph_.get();
    }
#endif
    const bool drawn = draw_scene(camera, current_settings, hdr_scheduler) &&
                       draw_diffuse_capture(current_settings) &&
                       (ui_ == nullptr || ui_->render(width_, height_));
    if (drawn)
    {
        draw_rvt_viewer();
        draw_vsm_viewer();
    }
    if (scene_resources_)
    {
        // 在扩展/UI 返回后撤销所有借用，下一次结构修改和 GPU 资源回收前不留旧指针。
        auto& frame = scene_resources_->frame_data;
        frame.meshes.clear();
        frame.lights.clear();
        frame.probes.clear();
        frame.world_epoch = 0;
        for (auto& list : scene_resources_->draw_lists)
        {
            list.batches.clear();
            list.opaque.clear();
            list.transparent.clear();
        }
    }
    if (!drawn && hdr_capture_status_ == SceneCaptureStatus::pending &&
        (!scene_resources_ || !scene_resources_->hdr_capture_pending))
    {
        hdr_capture_status_ = SceneCaptureStatus::failed;
    }
    {
        ENGINE_PROFILE_ZONE("Render.frameSubmit");
        {
            ENGINE_PROFILE_ZONE("Render.outputPresentSubmit");
            frame_output_->present(current_settings.clear_color, false);
        }
        if (drawn)
        {
            submit_screenshot(world_ && world_->scene() ? RenderFrameKind::scene
                                                        : RenderFrameKind::clear);
        }
        submit_frame();
        if (meshlet_culling_)
        {
            meshlet_culling_->poll(frame_number_);
        }
        if (vsm_pass_)
        {
            vsm_pass_->poll(frame_number_);
        }
        if (drawn)
        {
            record_frame(world_ && world_->scene() ? RenderFrameKind::scene
                                                   : RenderFrameKind::clear);
        }
    }
    {
        ENGINE_PROFILE_ZONE("Render.pollResults");
        if (drawn)
        {
            publish_features();
        }
        poll_diffuse_capture();
        poll_hdr_capture();
        collect_preparation(false);
    }
    ENGINE_PROFILE_FRAME_NAMED("Render");
#if defined(ENGINE_ENABLE_TRACY)
    profile_renderer_stats(rvt_status_, vsm_status_);
#endif
    return drawn ? RenderSubmitStatus::submitted : RenderSubmitStatus::failed;
}

RenderSubmitStatus RenderSystem::present_movie_frame(const MovieFrame& frame)
{
    ENGINE_PROFILE_FUNCTION();
    render_detail::ui_draws = 0;
    rvt_status_.effective = false;
    rvt_debug_visible_ = vsm_debug_visible_ = false;
    if (rvt_status_.requested)
    {
        rvt_status_.reason = "Scene view is not rendered";
    }
    const auto prepared = prepare_frame();
    if (prepared != render_detail::FramePrepareStatus::ready)
    {
        return prepared == render_detail::FramePrepareStatus::deferred
                   ? RenderSubmitStatus::deferred
                   : RenderSubmitStatus::failed;
    }
    if (!movie_pass_)
    {
        movie_pass_ = std::make_unique<render_detail::MoviePass>();
    }
    if (!movie_pass_->draw(frame, static_cast<std::uint16_t>(width_),
                           static_cast<std::uint16_t>(height_)))
    {
        return RenderSubmitStatus::failed;
    }
    frame_output_->present(0x000000ff, true);
    submit_screenshot(RenderFrameKind::movie);
    submit_frame();
    if (meshlet_culling_)
    {
        meshlet_culling_->poll(frame_number_);
    }
    if (vsm_pass_)
    {
        vsm_pass_->poll(frame_number_);
    }
    record_frame(RenderFrameKind::movie);
    collect_preparation(false);
    ENGINE_PROFILE_FRAME_NAMED("Render");
    return RenderSubmitStatus::submitted;
}

void RenderSystem::submit_frame() noexcept
{
    ENGINE_PROFILE_FUNCTION();
    {
        ENGINE_PROFILE_ZONE("Render.bgfxFrame");
        frame_number_ = bgfx::frame();
    }
#if defined(ENGINE_ENABLE_MEMORY_PROFILING)
    if (gpu_memory_)
    {
        gpu_memory_->frame_submitted(frame_number_);
    }
#endif
}

void RenderSystem::on_shutdown() noexcept
{
    ENGINE_PROFILE_FUNCTION();
    shutdown_renderer();
    preparation_.reset();
    if (surface_ != nullptr)
    {
        surface_->shutdown();
        surface_.reset();
    }
}

void RenderSystem::shutdown_renderer(bool drain_screenshots) noexcept
{
    ENGINE_PROFILE_FUNCTION();
    if (!render_detail::web_readbacks && drain_screenshots && renderer_initialized_ &&
        active_screenshot_)
    {
        for (int attempt = 0; attempt < 16 && active_screenshot_; ++attempt)
        {
            submit_frame();
            if (meshlet_culling_)
            {
                meshlet_culling_->poll(frame_number_);
            }
            if (vsm_pass_)
            {
                vsm_pass_->poll(frame_number_);
            }
            advance_screenshots();
            if (callbacks_)
            {
                callbacks_->finish();
            }
            advance_screenshots();
        }
    }
    for (auto& [id, operation] : screenshots_)
    {
        (void)id;
        if (operation.snapshot.state == ScreenshotState::waiting_frame ||
            operation.snapshot.state == ScreenshotState::reading_back ||
            operation.snapshot.state == ScreenshotState::writing)
        {
            operation.snapshot.state = ScreenshotState::failed;
            operation.snapshot.error_code = "device_unavailable";
            operation.snapshot.error = "Rendering device closed before the capture completed";
        }
    }
    scope_frames_.clear();
    release_scene();
    collect_preparation(true);
    invoking_extensions_ = true;
    for (auto* extension : extensions_)
    {
        extension->release_gpu();
    }
    invoking_extensions_ = false;
    if (renderer_initialized_)
    {
#if defined(ENGINE_ENABLE_TEST_SUPPORT)
        SDL_RemoveHintCallback(legacy_hdr_hint, update_hdr_scheduler, nullptr);
#endif
        render_graph_.reset();
        movie_pass_.reset();
        release_rvt();
        rvt_viewer_.reset();
        release_vsm();
        vsm_viewer_.reset();
        if (frame_output_)
        {
            frame_output_->release();
        }
        bgfx::shutdown();
#if defined(ENGINE_ENABLE_MEMORY_PROFILING)
        if (gpu_memory_)
        {
            gpu_memory_->device_drained();
        }
#endif
        meshlet_culling_.reset();
        vsm_pass_.reset();
        vsm_cache_.reset();
        frame_output_.reset();
        renderer_initialized_ = false;
        active_renderer = nullptr;
        frame_number_ = 0;
    }
    if (callbacks_ != nullptr)
    {
        callbacks_->finish();
    }
    const auto abandoned = screenshots_.find(active_screenshot_);
    active_screenshot_ = 0;
    if (abandoned != screenshots_.end())
    {
        std::error_code error;
        std::filesystem::remove(PathUtils::from_utf8(abandoned->second.temporary_path), error);
        if (abandoned->second.release_requested)
        {
            screenshots_.erase(abandoned);
        }
    }
    // GPU后台线程退出后，平台才可释放原生表面并完成暂停握手。
    if (surface_ != nullptr)
    {
        surface_->release();
    }
    width_ = 0;
    height_ = 0;
}

} // namespace engine
