#include "plugins/runtime/mcp/runtime_values.hpp"
#include "runtime/performance/function_profile.hpp"
#include "runtime/performance/profiling.hpp"
#include "runtime/app/runtime_host.hpp"
#include "runtime/core/guid_map.hpp"
#include "runtime/gameplay/gameplay_session.hpp"
#include "runtime/performance/memory_snapshot.hpp"
#include "runtime/render/gpu_memory_value.hpp"
#include "runtime/render/render_system.hpp"
#include "runtime/resource/resource_loader.hpp"
#include "runtime/task/task_system.hpp"
#include "runtime/utils/path_utils.hpp"
#include "runtime/world/world_system.hpp"
#include <SDL3/SDL.h>
#include <charconv>
#include <chrono>
#include <fstream>
#include <span>
#include <system_error>

namespace engine::plugins::mcp
{
using Json = nlohmann::json;
Json scene_load_value(const SceneLoadSnapshot& value)
{
    ENGINE_PROFILE_FUNCTION();
    constexpr const char* states[] = {"loading",    "staged", "preparing", "ready",
                                      "activating", "active", "failed",    "cancelled"};
    constexpr const char* work_states[] = {"queued",       "working", "ready",
                                           "not_required", "failed",  "cancelled"};
    auto stages = Json::array();
    for (const auto& stage : value.stages)
    {
        stages.push_back({{"domain", stage.domain},
                          {"stage", stage.stage},
                          {"state", work_states[static_cast<unsigned>(stage.state)]},
                          {"completed", std::to_string(stage.completed)},
                          {"total", std::to_string(stage.total)},
                          {"totalKnown", stage.total_known},
                          {"inFlight", std::to_string(stage.in_flight)},
                          {"progressRevision", std::to_string(stage.progress_revision)},
                          {"lastProgressNs", std::to_string(stage.last_progress_ns)},
                          {"elapsedNs", std::to_string(stage.elapsed_ns)},
                          {"errorCode", stage.error_code},
                          {"error", stage.error}});
    }
    return {{"id",
             {{"owner", std::to_string(value.id.owner)},
              {"request", std::to_string(value.id.request)}}},
            {"scope",
             {{"worldEpoch", std::to_string(value.scope.world_epoch)},
              {"value", std::to_string(value.scope.value)}}},
            {"state", states[static_cast<unsigned>(value.state)]},
            {"sceneId", std::to_string(value.scene_id)},
            {"targetAlive", value.target_alive},
            {"progressRevision", std::to_string(value.progress_revision)},
            {"lastProgressNs", std::to_string(value.last_progress_ns)},
            {"firstSceneSubmissionId", std::to_string(value.first_scene_submission_id)},
            {"stages", std::move(stages)},
            {"errorCode", value.error_code},
            {"error", value.error}};
}
Json frame_value(const RenderFrameSnapshot& value)
{
    ENGINE_PROFILE_FUNCTION();
    constexpr const char* kinds[] = {"scene", "movie", "clear"};
    return {{"deviceGeneration", std::to_string(value.device_generation)},
            {"submissionId", std::to_string(value.submission_id)},
            {"worldEpoch", std::to_string(value.world_epoch)},
            {"sceneId", std::to_string(value.scene_id)},
            {"contentRevision", std::to_string(value.content_revision)},
            {"submittedAtNs", std::to_string(value.submitted_at_ns)},
            {"kind", kinds[static_cast<unsigned>(value.kind)]},
            {"meshDraws", value.mesh_draws},
            {"uiDraws", value.ui_draws},
            {"rvtDemandRevision", std::to_string(value.rvt_demand_revision)},
            {"rvtCacheRevision", std::to_string(value.rvt_cache_revision)},
            {"rvtDebugRevision", std::to_string(value.rvt_debug_revision)},
            {"rvtReady", value.rvt_ready},
            {"rvtDebugVisible", value.rvt_debug_visible},
            {"vsmGeneration", std::to_string(value.vsm_generation)},
            {"vsmViewRevision", std::to_string(value.vsm_view_revision)},
            {"vsmDemandRevision", std::to_string(value.vsm_demand_revision)},
            {"vsmCacheRevision", std::to_string(value.vsm_cache_revision)},
            {"vsmDebugRevision", std::to_string(value.vsm_debug_revision)},
            {"vsmDebugVisible", value.vsm_debug_visible},
            {"vsmReady", value.vsm_ready}};
}
Json screenshot_value(const ScreenshotSnapshot& value)
{
    ENGINE_PROFILE_FUNCTION();
    constexpr const char* states[] = {"waiting_frame", "reading_back", "writing",
                                      "complete",      "failed",       "cancelled"};
    return {
        {"captureId",
         {{"owner", std::to_string(value.id.owner)}, {"value", std::to_string(value.id.value)}}},
        {"state", states[static_cast<unsigned>(value.state)]},
        {"frame", frame_value(value.frame)},
        {"path", value.path},
        {"errorCode", value.error_code},
        {"error", value.error}};
}
namespace
{
bool decimal(const Json& object, const char* field, std::uint64_t& output)
{
    ENGINE_PROFILE_FUNCTION();
    output = 0;
    if (!object.is_object() || !object.contains(field) || !object[field].is_string())
    {
        return false;
    }
    const auto& text = object[field].get_ref<const std::string&>();
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), output);
    return !text.empty() && parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

namespace
{
Json pool_memory_value(const memory_profile::PoolUsage& pool)
{
    ENGINE_PROFILE_FUNCTION();
    return {{"currentBytes", std::to_string(pool.current_bytes)},
            {"peakBytes", std::to_string(pool.peak_bytes)},
            {"liveAllocations", std::to_string(pool.live_allocations)}};
}

Json heap_records_value(std::span<const memory_profile::HeapRecord> records)
{
    ENGINE_PROFILE_FUNCTION();
    auto values = Json::array();
    for (const auto& record : records)
    {
        if (record.id)
        {
            values.push_back({{"id", std::to_string(record.id)},
                              {"requestedBytes", std::to_string(record.requested_bytes)},
                              {"releaseSequence", std::to_string(record.release_sequence)}});
        }
    }
    return values;
}

Json general_heap_value(const memory_profile::HeapSnapshot& heap)
{
    ENGINE_PROFILE_FUNCTION();
    const auto capacity = heap.recent_releases.size();
    const auto overwritten = heap.tracked_releases > capacity ? heap.tracked_releases - capacity : 0;
    return {{"available", heap.available},
            {"coverage", "allocatorEntryPoints"},
            {"sequence", std::to_string(heap.sequence)},
            {"unattributed", pool_memory_value(heap.unattributed)},
            {"attributed", pool_memory_value(heap.attributed)},
            {"trackedAllocations", std::to_string(heap.tracked_allocations)},
            {"trackedReleases", std::to_string(heap.tracked_releases)},
            {"overwrittenReleases", std::to_string(overwritten)},
            {"droppedAllocations", std::to_string(heap.dropped_allocations)},
            {"unmatchedReleases", std::to_string(heap.unmatched_releases)},
            {"unmatchedAttributions", std::to_string(heap.unmatched_attributions)},
            {"ownershipConflicts", std::to_string(heap.ownership_conflicts)},
            {"captureGaps", std::to_string(heap.capture_gaps)},
            {"recordCapacity", heap.record_capacity},
            {"metadataBytes", heap.metadata_bytes},
            {"largest", heap_records_value(heap.largest)},
            {"recentReleases", heap_records_value(heap.recent_releases)}};
}
} // namespace

Json cpu_memory_value()
{
    ENGINE_PROFILE_FUNCTION();
    const auto sample = memory_profile::snapshot();
    constexpr const char* modes[] = {"off", "totals", "detailed"};
    Json pools = Json::object();
    for (std::size_t index = 0; index < sample.pools.size(); ++index)
    {
        const auto& pool = sample.pools[index];
        auto value = pool_memory_value(pool);
        value["hasSamples"] = pool.peak_bytes != 0;
        pools[memory_profile::pool_keys[index]] = std::move(value);
    }
    Json data{{"mode", modes[static_cast<unsigned>(sample.mode)]},
              {"sequence", std::to_string(sample.sequence)},
              {"sampledAtNs", std::to_string(sample.timestamp_ns)},
              {"tracyConnectionId", std::to_string(sample.connection_id)},
              {"tracyConnected", sample.connected},
              {"captureGaps", std::to_string(sample.capture_gaps)},
              {"callstacksAvailable", sample.callstacks_available},
              {"pools", std::move(pools)}};
    if (sample.general_heap.available)
    {
        data["generalHeap"] = general_heap_value(sample.general_heap);
    }
    return data;
}

Json resource_memory_entry_value(const ResourceMemoryEntry& entry)
{
    ENGINE_PROFILE_FUNCTION();
    constexpr const char* phases[] = {"queued", "reading", "decoded",
                                      "ready",  "failed",  "retiring"};
    constexpr const char* holders[] = {"acquisition", "dependency", "load_request"};
    constexpr const char* owners[] = {"unspecified", "scene", "scene_scope", "scene_load"};
    Json references = Json::array();
    for (const auto& item : entry.holders)
    {
        references.push_back({{"kind", holders[static_cast<unsigned>(item.kind)]},
                              {"id", std::to_string(item.id)},
                              {"owner",
                               {{"kind", owners[static_cast<unsigned>(item.owner.kind)]},
                                {"domain", std::to_string(item.owner.domain)},
                                {"id", std::to_string(item.owner.id)}}},
                              {"guid", item.guid},
                              {"pendingRelease", item.pending_release}});
    }
    return {{"recordId", std::to_string(entry.record_id)},
            {"catalogGeneration", std::to_string(entry.catalog_generation)},
            {"guid", entry.guid},
            {"path", entry.path},
            {"kind", schema::EnumNameAssetKind(entry.kind)},
            {"phase", phases[static_cast<unsigned>(entry.phase)]},
            {"storedPayloadBytes", std::to_string(entry.stored_payload_bytes)},
            {"cpuPayloadBytes", entry.cpu_payload_bytes
                                    ? Json(std::to_string(*entry.cpu_payload_bytes))
                                    : Json(nullptr)},
            {"hasPayload", entry.has_payload},
            {"acquisitions", std::to_string(entry.acquisitions)},
            {"dependencyReferences", std::to_string(entry.dependency_references)},
            {"requestReferences", std::to_string(entry.request_references)},
            {"holdersTruncated", entry.holders_truncated},
            {"holders", std::move(references)}};
}

std::string counter_delta(std::uint64_t before, std::uint64_t after)
{
    ENGINE_PROFILE_FUNCTION();
    return after >= before ? std::to_string(after - before) : "-" + std::to_string(before - after);
}

bool resource_memory_observation(RuntimeHost& host, const Json& args, Json& result,
                                 std::string& error)
{
    ENGINE_PROFILE_FUNCTION();
    ResourceMemoryQuery query;
    for (auto it = args.begin(); it != args.end(); ++it)
    {
        if (it.key() == "guid" && it->is_string() &&
            GuidMap::valid_guid(it->get_ref<const std::string&>()))
        {
            query.guid = it->get_ref<const std::string&>();
        }
        else if (it.key() == "limit" && it->is_number_integer() && *it >= 1 &&
                 *it <= ResourceMemoryQuery::maximum_items)
        {
            query.limit = it->get<std::uint32_t>();
        }
        else if (it.key() != "baseline" || !it->is_object())
        {
            error = "Resource memory requires a GUID, a limit within 1.." +
                    std::to_string(ResourceMemoryQuery::maximum_items) +
                    " or a baseline totals object";
            return false;
        }
    }

    ResourceMemorySnapshot sample;
    const auto* world = host.world();
    const auto* loader = world ? world->resource_loader() : nullptr;
    if (!loader || !loader->memory_snapshot(query, sample))
    {
        error = "Resource memory observation is unavailable";
        return false;
    }
    const auto session =
        std::to_string(sample.loader_id) + "/" + std::to_string(sample.session_started_ns);
    Json delta = nullptr;
    if (const auto baseline = args.find("baseline"); baseline != args.end())
    {
        std::uint64_t sequence = 0, bytes = 0, measured = 0, unmeasured = 0;
        if (baseline->size() != 5 || !baseline->contains("sessionId") ||
            (*baseline)["sessionId"] != session || !decimal(*baseline, "sequence", sequence) ||
            sequence == 0 || sequence >= sample.sequence ||
            !decimal(*baseline, "measuredCpuPayloadBytes", bytes) ||
            !decimal(*baseline, "measuredPayloads", measured) ||
            !decimal(*baseline, "unmeasuredPayloads", unmeasured))
        {
            error = "Baseline must be the totals of an earlier snapshot from this loader session";
            return false;
        }
        delta = {
            {"fromSequence", std::to_string(sequence)},
            {"measuredCpuPayloadBytes", counter_delta(bytes, sample.measured_cpu_payload_bytes)},
            {"measuredPayloads", counter_delta(measured, sample.measured_payloads)},
            {"unmeasuredPayloads", counter_delta(unmeasured, sample.unmeasured_payloads)}};
    }

    Json entries = Json::array();
    for (const auto& entry : sample.entries)
    {
        entries.push_back(resource_memory_entry_value(entry));
    }
    result = {{"totals",
               {{"sessionId", session},
                {"sequence", std::to_string(sample.sequence)},
                {"measuredCpuPayloadBytes", std::to_string(sample.measured_cpu_payload_bytes)},
                {"measuredPayloads", std::to_string(sample.measured_payloads)},
                {"unmeasuredPayloads", std::to_string(sample.unmeasured_payloads)}}},
              {"sampledAtNs", std::to_string(sample.timestamp_ns)},
              {"catalogGeneration", std::to_string(sample.catalog_generation)},
              {"matchingEntries", std::to_string(sample.matching_entries)},
              {"truncated", sample.entries.size() < sample.matching_entries},
              {"entries", std::move(entries)},
              {"delta", std::move(delta)},
              {"cpuProfiler", cpu_memory_value()},
              {"gpuMemory", render_detail::gpu_memory_value(host.renderer(), query.guid, query.limit)}};
    return true;
}
} // namespace
bool publish_observation(TaskSystem* tasks, std::future<bool>& pending,
                         const std::filesystem::path& target, const Json& status, bool finish)
{
    ENGINE_PROFILE_FUNCTION();
    if (pending.valid())
    {
        if (!finish && pending.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        {
            return true;
        }
        if (!pending.get())
        {
            return false;
        }
    }
    auto temporary = target;
    temporary += ".mcp-tmp";
    auto write = [target, temporary, text = status.dump() + "\n"]()
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        output.close();
        return output && SDL_RenamePath(PathUtils::to_utf8(temporary).c_str(),
                                        PathUtils::to_utf8(target).c_str());
    };
    if (finish)
    {
        return write();
    }
    if (!tasks)
    {
        return false;
    }
    pending = tasks->submit("Mcp.statusWrite", std::move(write));
    // Bounded admission: retry on a later poll without blocking presentation.
    return true;
}

bool observation_command(RuntimeHost& host, const std::filesystem::path& cache,
                         std::string_view command, const Json& args, Json& result,
                         std::string& error)
{
    ENGINE_PROFILE_FUNCTION();
    result = Json::object();
    if (!args.is_object())
    {
        error = "Observation arguments must be an object";
        return false;
    }
    if (command == "get_resource_memory")
    {
        return resource_memory_observation(host, args, result, error);
    }
    if (command == "get_gameplay_prepare" || command == "release_gameplay_prepare")
    {
        std::uint64_t id = 0;
        GameplayPrepareSnapshot snapshot;
        auto* gameplay = host.gameplay();
        if (args.size() != 1 || !decimal(args, "prepareId", id) || id == 0 || !gameplay ||
            !gameplay->prepare_status(id, snapshot))
        {
            error = "Gameplay preparation identity is invalid";
            return false;
        }
        if (command == "release_gameplay_prepare")
        {
            if (!gameplay->release_prepare(id))
            {
                error = "Gameplay preparation is not terminal";
                return false;
            }
            result = {{"prepareId", std::to_string(id)}, {"released", true}};
            return true;
        }
        constexpr const char* states[] = {"preparing", "ready", "active", "failed", "cancelled"};
        result = {{"id", std::to_string(snapshot.id)},
                  {"state", states[static_cast<unsigned>(snapshot.state)]},
                  {"sceneLoadId",
                   {{"owner", std::to_string(snapshot.scene_load_id.owner)},
                    {"request", std::to_string(snapshot.scene_load_id.request)}}},
                  {"runId", std::to_string(snapshot.run_id)},
                  {"errorCode", snapshot.error_code},
                  {"error", snapshot.error}};
        return true;
    }
    if (command == "get_scene_load")
    {
        SceneLoadId id;
        SceneLoadSnapshot snapshot;
        if (!args.contains("loadId") || !decimal(args["loadId"], "owner", id.owner) ||
            !decimal(args["loadId"], "request", id.request) || !host.gameplay() ||
            !host.gameplay()->scene_load_status(id, snapshot))
        {
            error = "Scene loading identity is invalid";
            return false;
        }
        result = scene_load_value(snapshot);
        return true;
    }
    auto* renderer = host.renderer();
    if (!renderer)
    {
        error = "Frame capture requires an available renderer";
        return false;
    }
    if (command == "get_frame_capture")
    {
        ScreenshotId id;
        ScreenshotSnapshot snapshot;
        if (!args.contains("captureId") || !decimal(args["captureId"], "owner", id.owner) ||
            !decimal(args["captureId"], "value", id.value) ||
            !renderer->screenshot_status(id, snapshot))
        {
            error = "Capture identity is invalid";
            return false;
        }
        result = screenshot_value(snapshot);
        return true;
    }
    if (command != "capture_frame" || !args.contains("path") || !args["path"].is_string())
    {
        error = "Capture requires a relative cache PNG path";
        return false;
    }
    const auto text = args["path"].get<std::string>();
    const auto relative = PathUtils::from_utf8(text);
    if (text.empty() || text.find('\0') != std::string::npos ||
        text.find('\\') != std::string::npos || relative.is_absolute() ||
        relative.has_root_name() || relative.extension() != ".png")
    {
        error = "Screenshot path must be a relative PNG filename";
        return false;
    }
    for (const auto& part : relative)
    {
        if (part == "." || part == "..")
        {
            error = "Screenshot path cannot traverse directories";
            return false;
        }
    }
    std::filesystem::path current;
    for (const auto& part : cache / relative)
    {
        current /= part;
        std::error_code code;
        const auto status = std::filesystem::symlink_status(current, code);
        if (code == std::errc::no_such_file_or_directory)
        {
            continue;
        }
        if (code || std::filesystem::is_symlink(status))
        {
            error = "Screenshot path contains a symlink or inaccessible entry";
            return false;
        }
    }
    ScreenshotRequest request;
    request.path = PathUtils::to_utf8(cache / relative);
    const auto selection = args.value("selection", Json("next_frame"));
    if (selection != "next_frame" && selection != "scene_scope")
    {
        error = "Unknown screenshot selection";
        return false;
    }
    if (selection == "scene_scope")
    {
        request.selection = ScreenshotSelection::scene_scope;
        if (!args.contains("scope") ||
            !decimal(args["scope"], "worldEpoch", request.scope.world_epoch) ||
            !decimal(args["scope"], "value", request.scope.value) ||
            request.scope.world_epoch == 0 || request.scope.value == 0)
        {
            error = "Scene capture requires a nonempty scope";
            return false;
        }
    }
    if ((args.contains("deviceGeneration") &&
         !decimal(args, "deviceGeneration", request.device_generation)) ||
        (args.contains("afterSubmissionId") &&
         !decimal(args, "afterSubmissionId", request.after_submission_id)))
    {
        error = "Frame bounds must be uint64 decimal strings";
        return false;
    }
    std::error_code code;
    std::filesystem::create_directories((cache / relative).parent_path(), code);
    ScreenshotId id;
    ScreenshotSnapshot snapshot;
    if (code || !renderer->request_screenshot(request, id) ||
        !renderer->screenshot_status(id, snapshot))
    {
        error = "Renderer cannot accept this capture request";
        return false;
    }
    result = screenshot_value(snapshot);
    return true;
}

} // namespace engine::plugins::mcp
