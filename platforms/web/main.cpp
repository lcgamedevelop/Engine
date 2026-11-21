#include "runtime_entry.hpp"
#include "runtime/performance/function_profile.hpp"
#include "runtime/performance/profiling.hpp"

#include "runtime/gameplay/gameplay_session.hpp"
#include "runtime/render/render_system.hpp"
#include "runtime/scene/scene.hpp"
#include "runtime/world/world_system.hpp"
#include "web/render/web_surface_state.hpp"

#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_main.h>
#include <atomic>
#include <chrono>
#include <emscripten.h>
#include <memory>

#if defined(ENGINE_ENABLE_MEMORY_PROFILING)
#include "runtime/performance/memory_snapshot.hpp"
#include "runtime/render/gpu_memory_value.hpp"

#include <emscripten/heap.h>
#include <malloc.h>
#include <nlohmann/json.hpp>
#include <utility>
#endif

#if defined(ENGINE_ENABLE_WEB_HEAP_OBSERVATION)
#include "runtime/performance/heap_profiling.hpp"

#include <span>
#endif

namespace
{
std::unique_ptr<engine::host_detail::RuntimeAppState> app;
std::atomic<bool> exit_requested{false};
std::chrono::steady_clock::time_point previous;
bool stopping = false;
bool notified_ready = false;

#if defined(ENGINE_ENABLE_MEMORY_PROFILING)
std::atomic<bool> memory_snapshot_requested{false};

nlohmann::json pool_value(const engine::memory_profile::PoolUsage& pool)
{
    ENGINE_PROFILE_FUNCTION();
    return {{"currentBytes", pool.current_bytes},
            {"peakBytes", pool.peak_bytes},
            {"liveAllocations", pool.live_allocations}};
}

#if defined(ENGINE_ENABLE_WEB_HEAP_OBSERVATION)
nlohmann::json heap_records(std::span<const engine::memory_profile::HeapRecord> records)
{
    ENGINE_PROFILE_FUNCTION();
    auto values = nlohmann::json::array();
    for (const auto& record : records)
    {
        if (record.id != 0)
        {
            values.push_back({{"id", record.id},
                              {"requestedBytes", record.requested_bytes},
                              {"releaseSequence", record.release_sequence}});
        }
    }
    return values;
}

nlohmann::json heap_value(const engine::memory_profile::HeapSnapshot& heap)
{
    ENGINE_PROFILE_FUNCTION();
    const auto history_capacity = heap.recent_releases.size();
    const auto overwritten_releases =
        heap.tracked_releases > history_capacity ? heap.tracked_releases - history_capacity : 0;
    return {{"available", heap.available},
            {"coverage", "linkedCAllocatorCalls"},
            {"sequence", heap.sequence},
            {"unattributed", pool_value(heap.unattributed)},
            {"attributed", pool_value(heap.attributed)},
            {"trackedAllocations", heap.tracked_allocations},
            {"trackedReleases", heap.tracked_releases},
            {"overwrittenReleases", overwritten_releases},
            {"droppedAllocations", heap.dropped_allocations},
            {"unmatchedReleases", heap.unmatched_releases},
            {"unmatchedAttributions", heap.unmatched_attributions},
            {"ownershipConflicts", heap.ownership_conflicts},
            {"recordCapacity", heap.record_capacity},
            {"metadataBytes", heap.metadata_bytes},
            {"largest", heap_records(heap.largest)},
            {"recentReleases", heap_records(heap.recent_releases)}};
}
#endif

void publish_memory_snapshot()
{
    ENGINE_PROFILE_ZONE("Web.memorySnapshot");
    if (!memory_snapshot_requested.exchange(false, std::memory_order_relaxed))
    {
        return;
    }
#if defined(ENGINE_ENABLE_WEB_HEAP_OBSERVATION)
    const engine::memory_profile::heap::Pause pause;
#endif
    const auto memory = engine::memory_profile::snapshot();
    const auto allocator = mallinfo();
    auto pools = nlohmann::json::object();
    for (std::size_t index = 0; index < memory.pools.size(); ++index)
    {
        const auto& pool = memory.pools[index];
        auto value = pool_value(pool);
        value["hasSamples"] = pool.peak_bytes != 0;
        pools[engine::memory_profile::pool_keys[index]] = std::move(value);
    }
    const auto* world = app->host.world();
    const auto* scene = world ? world->scene() : nullptr;
    auto value = nlohmann::json{
        {"sequence", memory.sequence},
        {"timestampNs", std::to_string(memory.timestamp_ns)},
        {"connectionId", memory.connection_id},
        {"captureGaps", memory.capture_gaps},
        {"callstacksAvailable", memory.callstacks_available},
        {"sceneId", std::to_string(scene ? scene->id() : 0)},
        {"linearBytes", emscripten_get_heap_size()},
        {"allocatorLiveBytes", allocator.uordblks},
        {"allocatorFreeBytes", allocator.fordblks},
        {"pools", std::move(pools)},
        {"gpuMemory", engine::render_detail::gpu_memory_value(app->host.renderer(), {}, 32)},
    };
#if defined(ENGINE_ENABLE_WEB_HEAP_OBSERVATION)
    value["generalHeap"] = heap_value(memory.general_heap);
#endif
    const auto json = value.dump();
    MAIN_THREAD_EM_ASM({ Module.onEngineMemorySnapshot?.(JSON.parse(UTF8ToString($0))); },
                      json.c_str());
}
#endif

void stopped(int code, const std::string& error)
{
    ENGINE_PROFILE_ZONE("Web.notifyStopped");
    MAIN_THREAD_EM_ASM(
        {
            const code = $0;
            const error = UTF8ToString($1);
        queueMicrotask(() => Module.onEngineStopped?.(code, error));
        },
        code, error.c_str());
}

void frame()
{
    ENGINE_PROFILE_ZONE("Web.frame");
    if (stopping)
    {
        if (engine::render_detail::web_gpu_callbacks_pending())
        {
            return;
        }
        const auto code = app->exit_code;
        const auto error = app->error;
        app.reset();
        emscripten_cancel_main_loop();
        stopped(code, error);
        return;
    }
    if (exit_requested.load(std::memory_order_relaxed))
    {
        app->host.request_exit();
    }
    const auto now = std::chrono::steady_clock::now();
    const auto delta = std::chrono::duration<double>(now - previous).count();
    previous = now;
    const auto status = engine::host_detail::step_runtime_app(*app, delta);
#if defined(ENGINE_ENABLE_MEMORY_PROFILING)
    publish_memory_snapshot();
#endif
    if (status == engine::FrameStatus::paused)
    {
        previous = std::chrono::steady_clock::now();
        return;
    }
    if (status == engine::FrameStatus::failed || status == engine::FrameStatus::exit_requested)
    {
        engine::host_detail::shutdown_runtime_app(*app);
        stopping = true;
        return;
    }
    if (notified_ready || app->starting)
    {
        return;
    }
    engine::RuntimeHostStatus host;
    engine::GameplayPrepareSnapshot prepared;
    engine::RenderFrameSnapshot rendered;
    auto* gameplay = app->host.gameplay();
    auto* renderer = app->host.renderer();
    auto* world = app->host.world();
    if (!gameplay || !renderer || !world || !app->host.status(host) ||
        !gameplay->prepare_status(host.prepare_id, prepared) ||
        prepared.state != engine::GameplayPrepareStatus::active ||
        !renderer->frame_status({}, rendered) || rendered.kind != engine::RenderFrameKind::scene ||
        world->scene() == nullptr || world->scene()->id() != rendered.scene_id)
    {
        return;
    }
    if (prepared.scene_load_id != engine::SceneLoadId{})
    {
        engine::SceneLoadSnapshot load;
        if (!gameplay->scene_load_status(prepared.scene_load_id, load) ||
            !renderer->frame_status(load.scope, rendered))
        {
            return;
        }
    }
    notified_ready = true;
    const auto device = std::to_string(rendered.device_generation);
    const auto submission = std::to_string(rendered.submission_id);
    const auto scene = std::to_string(rendered.scene_id);
    MAIN_THREAD_EM_ASM(
        {
            Module.onEngineReady ?.({
                                      deviceGeneration : UTF8ToString($0),
                                      submissionId : UTF8ToString($1),
                                      sceneId : UTF8ToString($2)
                                  });
        },
        device.c_str(), submission.c_str(), scene.c_str());
}
} // namespace

extern "C" EMSCRIPTEN_KEEPALIVE void engine_web_request_exit()
{
    ENGINE_PROFILE_FUNCTION();
    exit_requested.store(true, std::memory_order_relaxed);
}

#if defined(ENGINE_ENABLE_MEMORY_PROFILING)
extern "C" EMSCRIPTEN_KEEPALIVE void engine_web_request_memory_snapshot()
{
    ENGINE_PROFILE_FUNCTION();
    memory_snapshot_requested.store(true, std::memory_order_relaxed);
}
#endif

int main(int argc, char** argv)
{
    ENGINE_PROFILE_FUNCTION();
#if defined(ENGINE_ENABLE_WEB_HEAP_OBSERVATION)
    engine::memory_profile::heap::allocations.start();
#endif
    {
        ENGINE_PROFILE_ZONE("Web.initialize");
        char selector[256]{};
        EM_ASM({ stringToUTF8("#" + Module.canvas.id, $0, $1); }, selector, sizeof(selector));
        SDL_SetHint(SDL_HINT_EMSCRIPTEN_CANVAS_SELECTOR, selector);
        app = std::make_unique<engine::host_detail::RuntimeAppState>();
        engine::AppConfig config;
        config.application_name = ENGINE_PROJECT_APPLICATION_NAME;
        if (!engine::host_detail::initialize_runtime_app(*app, config, argc, argv))
        {
            stopping = true;
        }
    }
    previous = std::chrono::steady_clock::now();
    emscripten_set_main_loop_arg([](void*)
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        frame();
    }, nullptr, 0, true);
    return 0;
}
