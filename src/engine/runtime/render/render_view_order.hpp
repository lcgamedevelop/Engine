#pragma once

#include "runtime/render/frame_graph_plan.hpp"

namespace engine::render_detail
{

// Compiles the reserved physical View slots, including slots written by editor jobs.
[[nodiscard]] bool compile_device_views(FrameGraphPlan& plan, std::uint16_t available_views,
                                        FramePlanError& error);

} // namespace engine::render_detail
