#include "plugins/runtime/terrain/terrain_component.hpp"
#include "runtime/performance/function_profile.hpp"
#include "runtime/performance/profiling.hpp"

#include "plugins/runtime/terrain/terrain_component_runtime.hpp"
#include "plugins/runtime/terrain/terrain_resource.hpp"
#include "plugins/runtime/terrain/terrain_samples.hpp"
#include "runtime/core/guid_map.hpp"
#include "runtime/scene/ecs_storage.hpp"
#include "runtime/scene/rigid_body.hpp"
#include "runtime/scene/transform.hpp"
#include "runtime/world/world_system.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>

namespace engine::plugins::terrain
{
namespace
{
bool valid_view(const TerrainView& view)
{
    ENGINE_PROFILE_FUNCTION();
    return terrain_detail::finite(view.center) && std::isfinite(view.visual_radius) &&
           std::isfinite(view.collision_radius) && view.visual_radius > 0 &&
           view.visual_radius <= 100000 && view.collision_radius >= 0 &&
           view.collision_radius <= view.visual_radius && view.lod_bias >= -15 &&
           view.lod_bias <= 15;
}

template <class Id> bool owner(TerrainComponent component, const Id& id)
{
    ENGINE_PROFILE_FUNCTION();
    return component && component.node().world()->epoch() == id.world_epoch &&
           component.node().id() == id.node_id &&
           component.instance_id() == id.component_instance && id.sequence != 0;
}

bool local_query(TerrainComponent component, const TerrainRegionQuery& query,
                 TerrainRegionQuery& result)
{
    ENGINE_PROFILE_FUNCTION();
    TransformSettings pose;
    if (!terrain_detail::valid_bounds(query.bounds) || component.node().parent() ||
        !component.node().get_component<Transform>().get_settings(pose) ||
        pose.rotation != Vec3{} || pose.scale.x() <= 0 || pose.scale.x() != pose.scale.y() ||
        pose.scale.x() != pose.scale.z())
    {
        return false;
    }
    const auto local = [&](Vec3 value)
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        return Vec3{(value.x() - pose.position.x()) / pose.scale.x(),
                    (value.y() - pose.position.y()) / pose.scale.x(),
                    (value.z() - pose.position.z()) / pose.scale.x()};
    };
    result = query;
    result.bounds = {local(query.bounds.minimum()), local(query.bounds.maximum())};
    return true;
}
} // namespace

bool TerrainComponent::register_type(ComponentRegistry& registry)
{
    ENGINE_PROFILE_FUNCTION();
    return register_component_storage<TerrainComponent, Data, Runtime>(registry);
}
TerrainComponent::Runtime* TerrainComponent::runtime() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return runtime_state<Runtime>();
}
bool TerrainComponent::valid_settings(const Data& value) noexcept
{
    ENGINE_PROFILE_FUNCTION();
    if ((!value.terrain_guid.empty() && !GuidMap::valid_guid(value.terrain_guid)) ||
        (!value.surface_material_guid.empty() &&
         !GuidMap::valid_guid(value.surface_material_guid)) ||
        value.rvt_role < schema::RvtRole::MIN || value.rvt_role > schema::RvtRole::MAX ||
        (value.initial_view && !valid_view(*value.initial_view)))
    {
        return false;
    }
    const auto budget = value.budget ? *value.budget : schema::TerrainBudgetDataT{};
    return budget.max_requests > 0 && budget.max_requests <= 1024 && budget.max_tasks > 0 &&
           budget.max_tasks <= 64 && budget.max_regions > 0 && budget.max_regions <= 4096 &&
           budget.cpu_bytes >= 1024 * 1024 && budget.gpu_bytes >= 1024 * 1024 &&
           budget.collision_bytes >= 1024 * 1024 && budget.upload_bytes_per_frame > 0 &&
           budget.publications_per_frame > 0 && budget.publications_per_frame <= 16;
}
bool TerrainComponent::get_settings(Data& output) const
{
    ENGINE_PROFILE_FUNCTION();
    const auto* data = read_data<Data>();
    output = data ? *data : Data{};
    return data != nullptr;
}
bool TerrainComponent::set_settings(const Data& value)
{
    ENGINE_PROFILE_FUNCTION();
    if (!writable() || !valid_settings(value) || runtime()->settings_revision_ == UINT64_MAX)
    {
        return false;
    }
    if (*read_data<Data>() == value)
    {
        return true;
    }
    *write_data<Data>() = value;
    auto* state = runtime();
    ++state->settings_revision_;
    state->error_.clear();
    state->error_code_.clear();
    state->plan_dirty_ = true;
    state->views_[0] = value.initial_view ? *value.initial_view : TerrainView{};
    modified();
    return true;
}
bool TerrainComponent::restore_data(const schema::ComponentDataT& data)
{
    ENGINE_PROFILE_FUNCTION();
    const auto* settings = data.payload.AsTerrainData();
    return settings && data.flags && set_settings(*settings) && restore_flags(data);
}
bool TerrainComponent::set_view(std::uint64_t key, const TerrainView& view)
{
    ENGINE_PROFILE_FUNCTION();
    if (!writable() || key == 0 || !valid_view(view))
    {
        return false;
    }
    auto* state = runtime();
    if (!state->views_.contains(key) && state->views_.size() >= 16)
    {
        return false;
    }
    if (const auto found = state->views_.find(key);
        found != state->views_.end() && found->second == view)
    {
        return true;
    }
    state->views_.erase(0);
    ++state->demand_revision_;
    state->views_[key] = view;
    state->error_.clear();
    state->error_code_.clear();
    state->plan_dirty_ = true;
    return true;
}
bool TerrainComponent::remove_view(std::uint64_t key)
{
    ENGINE_PROFILE_FUNCTION();
    if (!writable() || key == 0 || runtime()->views_.erase(key) == 0)
    {
        return false;
    }
    ++runtime()->demand_revision_;
    runtime()->plan_dirty_ = true;
    runtime()->error_.clear();
    runtime()->error_code_.clear();
    return true;
}
bool TerrainComponent::pin_regions(const TerrainPinSpec& spec, TerrainPinId& output)
{
    ENGINE_PROFILE_FUNCTION();
    output = {};
    if (!writable() || spec.owner_key == 0 || spec.regions.empty() || spec.regions.size() > 256 ||
        spec.reason.empty() || spec.reason.size() > 256)
    {
        return false;
    }
    auto* state = runtime();
    const auto* source = state->source_.data<TerrainResource>();
    if (!source || state->pins_.size() >= 64 || state->next_pin_ == UINT64_MAX)
    {
        return false;
    }
    for (auto key : spec.regions)
    {
        if (!terrain_detail::entry(*source, key) || (spec.collision_required && key.lod() != 0))
        {
            return false;
        }
    }
    const auto sequence = state->next_pin_++;
    state->pins_.emplace(sequence, spec);
    state->error_.clear();
    state->error_code_.clear();
    ++state->demand_revision_;
    state->plan_dirty_ = true;
    output = {node().world()->epoch(), node().id(), instance_id(), sequence};
    return true;
}
bool TerrainComponent::release_pin(TerrainPinId id)
{
    ENGINE_PROFILE_FUNCTION();
    if (!writable() || !owner(*this, id) || runtime()->pins_.erase(id.sequence) == 0)
    {
        return false;
    }
    ++runtime()->demand_revision_;
    runtime()->plan_dirty_ = true;
    runtime()->error_.clear();
    runtime()->error_code_.clear();
    return true;
}
bool TerrainComponent::request_edit(const TerrainEditSpec& spec, TerrainOperationId& output)
{
    ENGINE_PROFILE_FUNCTION();
    output = {};
    return writable() && runtime()->system_ && runtime()->system_->request(*this, spec, output);
}
bool TerrainComponent::request_collapse(const TerrainCollapseSpec& spec, TerrainOperationId& output)
{
    ENGINE_PROFILE_FUNCTION();
    output = {};
    return writable() && runtime()->system_ && runtime()->system_->request(*this, spec, output);
}
bool TerrainComponent::request_reset(const TerrainResetSpec& spec, TerrainOperationId& output)
{
    ENGINE_PROFILE_FUNCTION();
    output = {};
    return writable() && runtime()->system_ && runtime()->system_->request(*this, spec, output);
}
bool TerrainComponent::request_retire(const TerrainRetireSpec& spec, TerrainOperationId& output)
{
    ENGINE_PROFILE_FUNCTION();
    output = {};
    return writable() && runtime()->system_ && runtime()->system_->request(*this, spec, output);
}
bool TerrainComponent::request_save(const TerrainSaveSpec& spec, TerrainOperationId& output)
{
    ENGINE_PROFILE_FUNCTION();
    output = {};
    return writable() && runtime()->system_ && runtime()->system_->request(*this, spec, output);
}
bool TerrainComponent::request_load(const TerrainLoadSpec& spec, TerrainOperationId& output)
{
    ENGINE_PROFILE_FUNCTION();
    output = {};
    return writable() && runtime()->system_ && runtime()->system_->request(*this, spec, output);
}
bool TerrainComponent::operation_status(TerrainOperationId id,
                                        TerrainOperationSnapshot& output) const
{
    ENGINE_PROFILE_FUNCTION();
    output = {};
    if (!owner(*this, id))
    {
        return false;
    }
    const auto found = runtime()->operations_.find(id.sequence);
    if (found == runtime()->operations_.end())
    {
        return false;
    }
    output = found->second->snapshot;
    return true;
}
bool TerrainComponent::cancel_operation(TerrainOperationId id)
{
    ENGINE_PROFILE_FUNCTION();
    if (!writable() || !owner(*this, id))
    {
        return false;
    }
    const auto found = runtime()->operations_.find(id.sequence);
    if (found == runtime()->operations_.end() || terminal(found->second->snapshot.state) ||
        found->second->snapshot.state == TerrainOperationState::published)
    {
        return false;
    }
    auto& operation = *found->second;
    if (operation.persistence_commit)
    {
        return false;
    }
    operation.cancelled = true;
    if (operation.work)
    {
        operation.work->cancelled->store(true, std::memory_order_relaxed);
    }
    if (operation.geometry && !operation.geometry->cancel())
    {
        return false;
    }
    return true;
}
bool TerrainComponent::release_operation(TerrainOperationId id)
{
    ENGINE_PROFILE_FUNCTION();
    if (!writable() || !owner(*this, id))
    {
        return false;
    }
    auto& operations = runtime()->operations_;
    const auto found = operations.find(id.sequence);
    if (found == operations.end() || !terminal(found->second->snapshot.state) ||
        found->second->future.valid())
    {
        return false;
    }
    operations.erase(found);
    return true;
}
bool TerrainComponent::query_regions(const TerrainRegionQuery& query,
                                     TerrainRegionSnapshot& output) const
{
    ENGINE_PROFILE_FUNCTION();
    output = {};
    TerrainRegionQuery local;
    if (!valid() || query.limit == 0 || query.limit > 4096 ||
        !terrain_detail::valid_bounds(query.bounds))
    {
        return false;
    }
    const bool enabled = active();
    const auto* state = runtime();
    output.revision = state->revision_;
    output.settings_revision = state->settings_revision_;
    output.demand_revision = state->demand_revision_;
    output.published_settings_revision = state->prepared_settings_revision_;
    output.published_demand_revision = state->published_demand_revision_;
    output.fragment_count = state->fragments_.size();
    const auto* data = read_data<Data>();
    if (!enabled || data->terrain_guid.empty())
    {
        output.state = SceneWorkState::not_required;
    }
    else if (state->streaming_ &&
             state->streaming_->settings_revision == state->settings_revision_ &&
             state->streaming_->demand_revision == state->demand_revision_ &&
             state->streaming_->snapshot.state == TerrainOperationState::failed)
    {
        output.state = SceneWorkState::failed;
        output.error_code = state->streaming_->snapshot.error_code;
        output.error = state->streaming_->snapshot.error;
    }
    else if (!state->error_.empty())
    {
        output.state = SceneWorkState::failed;
        output.error_code = state->error_code_.empty() ? "streaming_failed" : state->error_code_;
        output.error = state->error_;
    }
    else
    {
        output.state = state->ready_ &&
                               state->prepared_settings_revision_ == state->settings_revision_ &&
                               state->published_demand_revision_ == state->demand_revision_
                           ? SceneWorkState::ready
                           : SceneWorkState::working;
    }
    const auto* source = state->source_.data<TerrainResource>();
    if (!source || !local_query(*this, query, local))
    {
        return query.cursor.empty() && (!source || output.state == SceneWorkState::failed ||
                                        output.state == SceneWorkState::not_required);
    }
    std::vector<TerrainRegionKey> keys;
    if (!terrain_detail::enumerate(*source, local.bounds, local.lod, 65536, keys))
    {
        return false;
    }
    std::size_t start = 0;
    if (!query.cursor.empty())
    {
        const auto split = query.cursor.find(':');
        if (split == std::string::npos)
        {
            return false;
        }
        std::uint64_t revision = 0;
        const auto* begin = query.cursor.data();
        const auto first = std::from_chars(begin, begin + split, revision);
        const auto second = std::from_chars(begin + split + 1, begin + query.cursor.size(), start);
        if (first.ec != std::errc{} || first.ptr != begin + split || second.ec != std::errc{} ||
            second.ptr != begin + query.cursor.size() || revision != state->revision_ ||
            start >= keys.size())
        {
            return false;
        }
    }
    const auto end = std::min(keys.size(), start + query.limit);
    for (auto index = start; index < end; ++index)
    {
        TerrainRegionStatus status;
        status.key = keys[index];
        if (const auto found = state->regions_.find(status.key); found != state->regions_.end())
        {
            status = found->second.status;
        }
        else
        {
            const auto* entry = terrain_detail::entry(*source, status.key);
            status.known = entry && entry->uniform != schema::TerrainUniformState::unknown;
            status.blocked_reason = status.known ? "not_resident" : "unknown_region";
        }
        status.dirty = state->dirty_.contains(status.key);
        for (const auto& [id, pin] : state->pins_)
        {
            (void)id;
            if (std::find(pin.regions.begin(), pin.regions.end(), status.key) != pin.regions.end())
            {
                ++status.pin_count;
            }
        }
        output.regions.push_back(std::move(status));
    }
    if (end < keys.size())
    {
        output.next_cursor = std::to_string(state->revision_) + ":" + std::to_string(end);
    }
    return true;
}
bool TerrainComponent::collect_support(const TerrainRegionQuery& query,
                                       std::vector<Collider>& output) const
{
    ENGINE_PROFILE_FUNCTION();
    output.clear();
    TerrainRegionSnapshot status;
    TerrainRegionQuery local;
    TerrainReadSnapshot samples;
    if (query.lod != 0 || !query.cursor.empty() || !query_regions(query, status) ||
        !status.next_cursor.empty() || !local_query(*this, query, local) ||
        !runtime()->volume_.read(local, samples))
    {
        return false;
    }
    std::vector<Collider> candidate;
    for (const auto& region : status.regions)
    {
        if (!region.known || region.state != SceneWorkState::ready || !region.collision_required ||
            region.mesh_revision != region.density_revision ||
            region.collision_revision != region.density_revision)
        {
            return false;
        }
        const auto found = runtime()->regions_.find(region.key);
        if (found == runtime()->regions_.end())
        {
            return false;
        }
        if (found->second.node)
        {
            auto collider = found->second.node.get_component<Collider>();
            schema::ColliderDataT settings;
            if (!collider || !collider.active() || !collider.get_settings(settings) ||
                settings.is_trigger || collider.node().get_component<RigidBody>())
            {
                return false;
            }
            candidate.push_back(collider);
        }
    }
    output = std::move(candidate);
    return true;
}
} // namespace engine::plugins::terrain
