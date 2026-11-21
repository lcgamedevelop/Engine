#include "plugins/runtime/terrain/terrain_control.hpp"
#include "runtime/performance/profiling.hpp"
#include "plugins/runtime/terrain/terrain_component_runtime.hpp"
#include "runtime/scene/collider.hpp"
#include "runtime/scene/ecs_storage.hpp"
#include "runtime/scene/node.hpp"
#include "runtime/schema/control_json_generated.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>

namespace engine::plugins::terrain::control
{
namespace
{
using Json = nlohmann::json;
template <class T> bool field(const Json& j, const char* name, T& value, std::string& error)
{
    ENGINE_PROFILE_FUNCTION();
    return !j.contains(name) || schema::control_json::read(j[name], value, error, false, name);
}
bool vector(const Json& j, Vec3& value, std::string& error)
{
    ENGINE_PROFILE_FUNCTION();
    return schema::control_json::read(j, value, error, false, "vector");
}
bool bounds(const Json& j, Bounds3& output, std::string& error)
{
    ENGINE_PROFILE_FUNCTION();
    Vec3 low, high;
    if (!j.is_object() || j.size() != 2 || !j.contains("minimum") || !j.contains("maximum") ||
        !vector(j["minimum"], low, error) || !vector(j["maximum"], high, error))
    {
        error = "Invalid terrain bounds";
        return false;
    }
    output = {low, high};
    return true;
}
bool key(const Json& j, TerrainRegionKey& value, std::string& error)
{
    ENGINE_PROFILE_FUNCTION();
    std::int32_t x = 0, y = 0, z = 0;
    std::uint8_t lod = 0;
    if (!j.is_object() || !j.contains("x") || !j.contains("y") || !j.contains("z") ||
        !j.contains("lod") || j.size() != 4 || !field(j, "x", x, error) ||
        !field(j, "y", y, error) || !field(j, "z", z, error) || !field(j, "lod", lod, error) ||
        lod > 15)
    {
        return false;
    }
    value = {x, y, z, lod};
    return true;
}
Json key_json(TerrainRegionKey value)
{
    ENGINE_PROFILE_FUNCTION();
    return Json{{"x", value.x()}, {"y", value.y()}, {"z", value.z()}, {"lod", value.lod()}};
}
Json bounds_json(const Bounds3& value)
{
    ENGINE_PROFILE_FUNCTION();
    return Json{{"minimum", schema::control_json::write(value.minimum(), false)},
                {"maximum", schema::control_json::write(value.maximum(), false)}};
}
template <class Id> bool identity(const Json& j, Id& id, std::string& error)
{
    ENGINE_PROFILE_FUNCTION();
    return j.is_object() && j.size() == 4 && j.contains("world_epoch") && j.contains("node_id") &&
           j.contains("component_instance") && j.contains("sequence") &&
           field(j, "world_epoch", id.world_epoch, error) &&
           field(j, "node_id", id.node_id, error) &&
           field(j, "component_instance", id.component_instance, error) &&
           field(j, "sequence", id.sequence, error);
}
template <class Id> Json id_json(Id id)
{
    ENGINE_PROFILE_FUNCTION();
    return Json{{"world_epoch", std::to_string(id.world_epoch)},
                {"node_id", std::to_string(id.node_id)},
                {"component_instance", std::to_string(id.component_instance)},
                {"sequence", std::to_string(id.sequence)}};
}
bool query(const Json& j, TerrainRegionQuery& value, std::string& error)
{
    ENGINE_PROFILE_FUNCTION();
    return j.contains("bounds") && bounds(j["bounds"], value.bounds, error) &&
           field(j, "lod", value.lod, error) && field(j, "limit", value.limit, error) &&
           field(j, "cursor", value.cursor, error);
}
bool brush(const Json& j, schema::TerrainBrushDataT& value, std::string& error)
{
    ENGINE_PROFILE_FUNCTION();
    if (!j.is_object() || !j.contains("center") || !j.contains("extent") ||
        !vector(j["center"], value.center, error) || !vector(j["extent"], value.extent, error) ||
        !field(j, "strength", value.strength, error))
    {
        return false;
    }
    std::string shape = "sphere", operation = "dig";
    std::uint32_t layer = 0;
    if (!field(j, "shape", shape, error) || !field(j, "operation", operation, error) ||
        !field(j, "layer", layer, error) || layer > 255)
    {
        return false;
    }
    if (shape == "sphere")
    {
        value.shape = schema::TerrainBrushShape::sphere;
    }
    else if (shape == "box")
    {
        value.shape = schema::TerrainBrushShape::box;
    }
    else
    {
        return false;
    }
    bool found = false;
    for (auto op : schema::EnumValuesTerrainBrushOperation())
    {
        if (operation == schema::EnumNameTerrainBrushOperation(op))
        {
            value.operation = op;
            found = true;
            break;
        }
    }
    value.layer = {layer, 0, 0, 0, Vec4{1, 0, 0, 0}};
    return found;
}
constexpr const char* states[]{"queued",    "waiting",   "preparing", "ready",
                               "published", "completed", "cancelled", "failed"};
constexpr const char* work_states[]{"queued",       "working", "ready",
                                    "not_required", "failed",  "cancelled"};
} // namespace
void abandon_operation(TerrainComponent component, const Json& value)
{
    ENGINE_PROFILE_FUNCTION();
    TerrainOperationId id;
    TerrainOperationSnapshot snapshot;
    std::string error;
    if (!identity(value, id, error) || !component.operation_status(id, snapshot))
    {
        return;
    }
    if (component.release_operation(id))
    {
        return;
    }
    (void)component.cancel_operation(id);
    auto* runtime = ecs_detail::component_runtime(component);
    if (runtime)
    {
        const auto found = runtime->operations_.find(id.sequence);
        if (found != runtime->operations_.end())
        {
            found->second->discard_when_terminal = true;
        }
    }
}
bool call(TerrainComponent component, std::string_view method, const Json& args, Json& output,
          std::string& error)
{
    ENGINE_PROFILE_FUNCTION();
    output = nullptr;
    error.clear();
    if (!component || !args.is_object())
    {
        error = "Invalid terrain owner or arguments";
        return false;
    }
    if (method == "get_settings")
    {
        schema::TerrainDataT value;
        if (!component.get_settings(value))
        {
            return false;
        }
        output = schema::control_json::write(value);
        return true;
    }
    if (method == "set_settings")
    {
        schema::TerrainDataT value;
        if (!schema::control_json::read(args, value, error) || !component.set_settings(value))
        {
            return false;
        }
        output = true;
        return true;
    }
    if (method == "set_view")
    {
        std::uint64_t id = 0;
        TerrainView value;
        if (!field(args, "view_key", id, error) || !args.contains("view") ||
            !schema::control_json::read(args["view"], value, error) ||
            !component.set_view(id, value))
        {
            return false;
        }
        output = true;
        return true;
    }
    if (method == "remove_view")
    {
        std::uint64_t id = 0;
        if (!field(args, "view_key", id, error) || !component.remove_view(id))
        {
            return false;
        }
        output = true;
        return true;
    }
    if (method == "pin_regions")
    {
        TerrainPinSpec spec;
        TerrainPinId id;
        if (!field(args, "owner_key", spec.owner_key, error) ||
            !field(args, "reason", spec.reason, error) ||
            !field(args, "collision_required", spec.collision_required, error) ||
            !args.contains("regions") || !args["regions"].is_array() ||
            args["regions"].size() > 256)
        {
            return false;
        }
        for (const auto& item : args["regions"])
        {
            TerrainRegionKey region;
            if (!key(item, region, error))
            {
                return false;
            }
            spec.regions.push_back(region);
        }
        if (!component.pin_regions(spec, id))
        {
            return false;
        }
        output = id_json(id);
        return true;
    }
    if (method == "release_pin")
    {
        TerrainPinId id;
        if (!identity(args, id, error) || !component.release_pin(id))
        {
            return false;
        }
        output = true;
        return true;
    }
    if (method == "operation_status" || method == "cancel_operation" ||
        method == "release_operation")
    {
        TerrainOperationId id;
        if (!identity(args, id, error))
        {
            return false;
        }
        if (method == "cancel_operation")
        {
            output = component.cancel_operation(id);
            return output.get<bool>();
        }
        if (method == "release_operation")
        {
            output = component.release_operation(id);
            return output.get<bool>();
        }
        TerrainOperationSnapshot status;
        if (!component.operation_status(id, status))
        {
            error = "Terrain operation owner or identity is no longer valid";
            return false;
        }
        output = {{"id", id_json(id)},
                  {"state", states[std::size_t(status.state)]},
                  {"base_revision", std::to_string(status.base_revision)},
                  {"published_revision", std::to_string(status.published_revision)},
                  {"content_revision", std::to_string(status.content_revision)},
                  {"dirty_bounds", bounds_json(status.dirty_bounds)},
                  {"created_fragments", status.created_fragments},
                  {"retired_fragments", status.retired_fragments},
                  {"checkpoint_id", status.checkpoint_id},
                  {"captured_revision", std::to_string(status.captured_revision)},
                  {"error_code", status.error_code},
                  {"error", status.error},
                  {"stages", Json::array()}};
        for (const auto& stage : status.stages)
        {
            output["stages"].push_back({{"stage", stage.stage},
                                        {"state", work_states[std::size_t(stage.state)]},
                                        {"completed", std::to_string(stage.completed)},
                                        {"total", std::to_string(stage.total)},
                                        {"total_known", stage.total_known}});
        }
        return true;
    }
    if (method == "query_regions" || method == "collect_support")
    {
        TerrainRegionQuery request;
        if (!query(args, request, error))
        {
            return false;
        }
        if (method == "collect_support")
        {
            std::vector<Collider> colliders;
            if (!component.collect_support(request, colliders))
            {
                error = "Terrain support is incomplete or has stale collision";
                return false;
            }
            output = Json::array();
            for (auto collider : colliders)
            {
                output.push_back({{"node_id", std::to_string(collider.node().id())},
                                  {"component_instance", std::to_string(collider.instance_id())}});
            }
            return true;
        }
        TerrainRegionSnapshot status;
        if (!component.query_regions(request, status))
        {
            return false;
        }
        output = {
            {"revision", std::to_string(status.revision)},
            {"state", work_states[std::size_t(status.state)]},
            {"settings_revision", std::to_string(status.settings_revision)},
            {"demand_revision", std::to_string(status.demand_revision)},
            {"published_settings_revision", std::to_string(status.published_settings_revision)},
            {"published_demand_revision", std::to_string(status.published_demand_revision)},
            {"error_code", status.error_code},
            {"error", status.error},
            {"fragment_count", std::to_string(status.fragment_count)},
            {"next_cursor", status.next_cursor},
            {"regions", Json::array()}};
        for (const auto& region : status.regions)
        {
            output["regions"].push_back(
                {{"key", key_json(region.key)},
                 {"density_revision", std::to_string(region.density_revision)},
                 {"mesh_revision", std::to_string(region.mesh_revision)},
                 {"collision_revision", std::to_string(region.collision_revision)},
                 {"collision_required", region.collision_required},
                 {"known", region.known},
                 {"dirty", region.dirty},
                 {"pin_count", region.pin_count},
                 {"state", work_states[std::size_t(region.state)]},
                 {"blocked_reason", region.blocked_reason}});
        }
        return true;
    }
    TerrainOperationId id;
    bool accepted = false;
    if (method == "request_edit")
    {
        TerrainEditSpec spec;
        if (!field(args, "expected_revision", spec.expected_revision, error) ||
            !args.contains("brushes") || !args["brushes"].is_array() || args["brushes"].size() > 64)
        {
            return false;
        }
        for (const auto& item : args["brushes"])
        {
            schema::TerrainBrushDataT value;
            if (!brush(item, value, error))
            {
                return false;
            }
            spec.brushes.push_back(std::move(value));
        }
        accepted = component.request_edit(spec, id);
    }
    else if (method == "request_collapse")
    {
        TerrainCollapseSpec spec;
        if (!args.contains("bounds") || !bounds(args["bounds"], spec.bounds, error) ||
            !field(args, "expected_revision", spec.expected_revision, error) ||
            !field(args, "min_samples", spec.min_samples, error) ||
            !field(args, "fragment_cells", spec.fragment_cells, error) ||
            !field(args, "max_fragments", spec.max_fragments, error) ||
            !field(args, "density", spec.density, error) ||
            !field(args, "connectivity_block_limit", spec.connectivity_block_limit, error))
        {
            return false;
        }
        accepted = component.request_collapse(spec, id);
    }
    else if (method == "request_reset")
    {
        TerrainResetSpec spec;
        if (!args.contains("bounds") || !bounds(args["bounds"], spec.bounds, error) ||
            !field(args, "expected_revision", spec.expected_revision, error))
        {
            return false;
        }
        accepted = component.request_reset(spec, id);
    }
    else if (method == "request_retire")
    {
        TerrainRetireSpec spec;
        if (!field(args, "expected_revision", spec.expected_revision, error) ||
            !field(args, "max_count", spec.max_count, error) ||
            !field(args, "max_age_seconds", spec.max_age_seconds, error))
        {
            return false;
        }
        accepted = component.request_retire(spec, id);
    }
    else if (method == "request_save")
    {
        TerrainSaveSpec spec;
        if (!field(args, "expected_revision", spec.expected_revision, error) ||
            !field(args, "slot", spec.slot, error) ||
            !field(args, "checkpoint_id", spec.checkpoint_id, error))
        {
            return false;
        }
        accepted = component.request_save(spec, id);
    }
    else if (method == "request_load")
    {
        TerrainLoadSpec spec;
        if (!field(args, "expected_base_hash", spec.expected_base_hash, error) ||
            !field(args, "slot", spec.slot, error) ||
            !field(args, "checkpoint_id", spec.checkpoint_id, error))
        {
            return false;
        }
        accepted = component.request_load(spec, id);
    }
    else
    {
        error = "Unknown terrain operation";
        return false;
    }
    if (!accepted)
    {
        error =
            "Terrain request rejected: owner not ready, invalid input, or request budget exhausted";
        return false;
    }
    output = id_json(id);
    return true;
}
} // namespace engine::plugins::terrain::control
