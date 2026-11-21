#include "runtime/engine.hpp"
#include "runtime/performance/function_profile.hpp"

#include "runtime/engine_config.hpp"
#include "runtime/system/system.hpp"

#include <cmath>
#include <utility>

namespace engine
{

Engine::Engine() noexcept = default;

Engine::~Engine() noexcept
{
    ENGINE_PROFILE_FUNCTION_PROBE();
    shutdown();
}

Engine& Engine::instance() noexcept
{
    ENGINE_PROFILE_FUNCTION_PROBE();
    static Engine engine;
    return engine;
}

bool Engine::add_system(std::unique_ptr<System>& system)
{
    ENGINE_PROFILE_FUNCTION_PROBE();
    if (initialized_ || executing_ || system == nullptr || system->parent() != nullptr ||
        system->is_initialized())
    {
        return false;
    }
    systems_.push_back(std::move(system));
    return true;
}

std::size_t Engine::system_count() const noexcept
{
    ENGINE_PROFILE_FUNCTION_PROBE();
    return systems_.size();
}

bool Engine::is_initialized() const noexcept
{
    ENGINE_PROFILE_FUNCTION_PROBE();
    return initialized_;
}

bool Engine::initialize(const EngineConfig& config)
{
    ENGINE_PROFILE_FUNCTION_PROBE();
    if (initialized_ || executing_ || systems_.empty() ||
        !frame_limiter_.set_target_frame_rate(config.target_frame_rate))
    {
        return false;
    }
    executing_ = true;
    for (const auto& system : systems_)
    {
        if (!system->initialize(config))
        {
            executing_ = false;
            shutdown();
            return false;
        }
    }
    initialized_ = true;
    executing_ = false;
    return true;
}

bool Engine::set_target_frame_rate(std::int32_t rate) noexcept
{
    ENGINE_PROFILE_FUNCTION_PROBE();
    return frame_limiter_.set_target_frame_rate(rate);
}

std::int32_t Engine::target_frame_rate() const noexcept
{
    ENGINE_PROFILE_FUNCTION_PROBE();
    return frame_limiter_.target_frame_rate();
}

void Engine::begin_frame() noexcept
{
    ENGINE_PROFILE_FUNCTION_PROBE();
    frame_limiter_.begin_frame();
}

void Engine::end_frame()
{
    ENGINE_PROFILE_FUNCTION_PROBE();
    frame_limiter_.end_frame();
}

bool Engine::tick(double delta_time)
{
    ENGINE_PROFILE_FUNCTION_PROBE();
    if (!initialized_ || executing_ || failed_ || !std::isfinite(delta_time) || delta_time < 0.0)
    {
        return false;
    }
    executing_ = true;
    for (const auto& system : systems_)
    {
        if (!system->tick(delta_time))
        {
            failed_ = true;
            executing_ = false;
            return false;
        }
    }
    executing_ = false;
    return true;
}

void Engine::shutdown() noexcept
{
    ENGINE_PROFILE_FUNCTION_PROBE();
    if (executing_)
    {
        return;
    }
    executing_ = true;
    while (!systems_.empty())
    {
        systems_.back()->shutdown();
        systems_.pop_back();
    }
    initialized_ = false;
    failed_ = false;
    frame_limiter_.reset();
    executing_ = false;
}

} // namespace engine
