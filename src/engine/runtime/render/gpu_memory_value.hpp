#pragma once

#include "runtime/performance/function_profile.hpp"
#include "runtime/render/gpu_memory.hpp"
#include "runtime/render/render_system.hpp"

#include <nlohmann/json.hpp>

namespace engine::render_detail
{
inline nlohmann::json gpu_memory_value(const RenderSystem* renderer, std::string_view guid,
                                      std::uint32_t limit)
{
    ENGINE_PROFILE_FUNCTION_PROBE();
    using Json = nlohmann::json;
    GpuMemorySnapshot sample;
    if (!renderer || !renderer->memory_snapshot(guid, limit, sample))
    {
        return {{"available", false}};
    }
    const auto point = [](GpuMemoryPoint timestamp) -> Json
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        if (!timestamp.timestamp_ns)
        {
            return nullptr;
        }
        return {{"frame", timestamp.frame}, {"timestampNs", std::to_string(timestamp.timestamp_ns)}};
    };
    constexpr const char* kinds[] = {"texture", "mesh_geometry", "static_vertex_page",
                                     "frame_output", "frame_readback"};
    constexpr const char* phases[] = {"live",           "waiting_readback", "waiting_retirement",
                                      "destroy_queued", "destroy_submitted", "backend_reclaimed"};
    const auto entries = [&](const auto& rows)
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        auto values = Json::array();
        for (const auto& entry : rows)
        {
            values.push_back(
                {{"id", std::to_string(entry.id)},
                 {"kind", kinds[static_cast<unsigned>(entry.kind)]},
                 {"phase", phases[static_cast<unsigned>(entry.phase)]},
                 {"guid", entry.guid},
                 {"creationScope",
                  {{"worldEpoch", std::to_string(entry.creation_scope.world_epoch)},
                   {"value", std::to_string(entry.creation_scope.value)}}},
                 {"creationScene", std::to_string(entry.creation_scene)},
                 {"logicalBytes", std::to_string(entry.logical_bytes)},
                 {"sharedBackingBytes", std::to_string(entry.shared_backing_bytes)},
                 {"sharedBackingId", std::to_string(entry.shared_backing_id)},
                 {"created", point(entry.created)},
                 {"releaseRequested", point(entry.release_requested)},
                 {"readbackReady", point(entry.readback_ready)},
                 {"destroyQueued", point(entry.destroy_queued)},
                 {"destroySubmitted", point(entry.destroy_submitted)},
                 {"backendReclaimed", point(entry.backend_reclaimed)},
                 {"drainedOnDeviceClose", entry.drained_on_device_close}});
        }
        return values;
    };
    const auto& totals = sample.totals;
    return {
        {"available", true},
        {"sessionId", std::to_string(sample.device_generation) + "/" +
                          std::to_string(sample.session_started_ns)},
        {"sequence", std::to_string(sample.sequence)},
        {"deviceGeneration", std::to_string(sample.device_generation)},
        {"sampled", point(sample.sampled)},
        {"coverage",
         {{"kinds", kinds},
          {"logicalBytes", "estimate"},
          {"sharedBackingBytes", "requested_capacity"},
          {"driverBytes", sample.driver_bytes ? "reported" : "unavailable"},
          {"gpuCompletion", "readback_only"}}},
        {"totals",
         {{"liveCount", std::to_string(totals.live_count)},
          {"liveLogicalBytes", std::to_string(totals.live_logical_bytes)},
          {"liveSharedBackingBytes", std::to_string(totals.live_shared_backing_bytes)},
          {"retiringCount", std::to_string(totals.retiring_count)},
          {"retiringLogicalBytes", std::to_string(totals.retiring_logical_bytes)},
          {"retiringSharedBackingBytes", std::to_string(totals.retiring_shared_backing_bytes)},
          {"peakLogicalBytes", std::to_string(totals.peak_logical_bytes)},
          {"peakSharedBackingBytes", std::to_string(totals.peak_shared_backing_bytes)},
          {"driverBytes", sample.driver_bytes ? Json(std::to_string(*sample.driver_bytes))
                                              : Json(nullptr)},
          {"textureStagingReservedBytes", std::to_string(sample.texture_staging_reserved_bytes)},
          {"retiredTextureStagingReservedBytes",
           std::to_string(sample.retired_texture_staging_reserved_bytes)}}},
        {"live", entries(sample.live)},
        {"pending", entries(sample.pending)},
        {"recentReclaims", entries(sample.recent_reclaims)},
        {"matchingLive", std::to_string(sample.matching_live)},
        {"matchingPending", std::to_string(sample.matching_pending)},
        {"matchingReclaims", std::to_string(sample.matching_reclaims)},
        {"overwrittenHistory", std::to_string(sample.overwritten_history)}};
}
} // namespace engine::render_detail
