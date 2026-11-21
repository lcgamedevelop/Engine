#pragma once

#include "runtime/performance/function_profile.hpp"
#include <cstdint>
namespace engine::render_detail
{
inline thread_local std::uint32_t ui_draws = 0;
inline thread_local bool collecting_ui = false;
struct UiDrawScope
{
    UiDrawScope()
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        ui_draws = 0;
        collecting_ui = true;
    }
    ~UiDrawScope()
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        collecting_ui = false;
    }
};
} // namespace engine::render_detail
