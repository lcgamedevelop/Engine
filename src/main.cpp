#include "runtime/performance/profiling.hpp"
#include "runtime_entry.hpp"

#if defined(ENGINE_HAS_ANDROID_MEMORY_COUNTERS)
#include "android/native/memory_counters.hpp"
#endif

#include <SDL3/SDL_main.h>
#include <chrono>
#include <thread>

int main(int argc, char* argv[])
{
    ENGINE_PROFILE_FUNCTION();
    engine::host_detail::RuntimeAppState state;
    engine::AppConfig config;
#if defined(ENGINE_PROJECT_APPLICATION_NAME)
    config.application_name = ENGINE_PROJECT_APPLICATION_NAME;
#endif
    if (!engine::host_detail::initialize_runtime_app(state, config, argc, argv))
    {
        return state.exit_code;
    }
    auto previous = std::chrono::steady_clock::now();
#if defined(ENGINE_HAS_ANDROID_MEMORY_COUNTERS)
    auto next_memory_sample = previous;
#endif
    for (;;)
    {
        const auto now = std::chrono::steady_clock::now();
        const auto delta = std::chrono::duration<double>(now - previous).count();
        previous = now;
        const auto status = engine::host_detail::step_runtime_app(state, delta);
        if (status == engine::FrameStatus::failed || status == engine::FrameStatus::exit_requested)
        {
            break;
        }
#if defined(ENGINE_HAS_ANDROID_MEMORY_COUNTERS)
        if (now >= next_memory_sample)
        {
            engine::host_detail::publish_android_memory_counters(state.host);
            next_memory_sample = now + std::chrono::seconds(1);
        }
#endif
        if (status == engine::FrameStatus::paused)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            previous = std::chrono::steady_clock::now();
        }
    }
    engine::host_detail::shutdown_runtime_app(state);
    return state.exit_code;
}
