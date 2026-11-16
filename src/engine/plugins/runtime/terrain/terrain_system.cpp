#include "plugins/runtime/terrain/terrain_system.hpp"

#include "plugins/runtime/terrain/terrain_component_runtime.hpp"
#include "plugins/runtime/terrain/terrain_fragment_runtime.hpp"
#include "plugins/runtime/terrain/terrain_islands.hpp"
#include "plugins/runtime/terrain/terrain_resource.hpp"
#include "plugins/runtime/terrain/terrain_samples.hpp"
#include "runtime/app/app.hpp"
#include "runtime/core/guid_map.hpp"
#include "runtime/performance/profiling.hpp"
#include "runtime/physics/physics_system.hpp"
#include "runtime/render/render_system.hpp"
#include "runtime/resource/types/mesh_resource.hpp"
#include "runtime/scene/ecs_storage.hpp"
#include "runtime/scene/rigid_body.hpp"
#include "runtime/scene/scene.hpp"
#include "runtime/task/task_system.hpp"
#include "runtime/utils/hash_utils.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <flatbuffers/flatbuffers.h>
#include <set>
#include <type_traits>

namespace engine::plugins::terrain
{
namespace
{
using namespace terrain_detail;
using Keys = std::set<TerrainRegionKey, TerrainRegionLess>;
using Runtime = TerrainComponentRuntime;

bool pose_of(TerrainComponent component, TransformSettings& pose)
{
    return !component.node().parent() &&
           component.node().get_component<Transform>().get_settings(pose) &&
           finite(pose.position) && pose.rotation == Vec3{} && finite(pose.scale) &&
           pose.scale.x() > 0 && pose.scale.x() == pose.scale.y() &&
           pose.scale.x() == pose.scale.z();
}
Vec3 local_point(Vec3 value, const TransformSettings& pose)
{
    return {(value.x() - pose.position.x()) / pose.scale.x(),
            (value.y() - pose.position.y()) / pose.scale.x(),
            (value.z() - pose.position.z()) / pose.scale.x()};
}
Bounds3 local_bounds(const Bounds3& value, const TransformSettings& pose)
{
    return {local_point(value.minimum(), pose), local_point(value.maximum(), pose)};
}
Vec3 global_point(Vec3 value, const TransformSettings& pose)
{
    return {value.x() * pose.scale.x() + pose.position.x(),
            value.y() * pose.scale.x() + pose.position.y(),
            value.z() * pose.scale.x() + pose.position.z()};
}
void fail(TerrainOperation& operation, std::string code, std::string error)
{
    operation.snapshot.state = TerrainOperationState::failed;
    operation.snapshot.error_code = std::move(code);
    operation.snapshot.error = std::move(error);
    for (auto& stage : operation.snapshot.stages)
    {
        stage.state = operation.snapshot.error_code == "cancelled" ? SceneWorkState::cancelled
                                                                   : SceneWorkState::failed;
    }
    if (operation.geometry)
    {
        operation.geometry->release();
    }
    operation.geometry.reset();
    if (operation.scope_world && operation.new_scope.value)
    {
        operation.scope_world->release_scope(operation.new_scope);
        operation.new_scope = {};
    }
    operation.loads.clear();
}
void progress(TerrainOperation& operation, std::string stage, std::uint64_t done,
              std::uint64_t total, SceneWorkState state)
{
    SceneWorkProgress value;
    value.domain = "terrain";
    value.stage = std::move(stage);
    value.completed = done;
    value.total = total;
    value.total_known = true;
    value.state = state;
    operation.snapshot.stages.assign(1, std::move(value));
}

// A ready ResourceLoad transfers its own reference. No synchronous file load occurs here.
SceneWorkState load(ResourceLoader& loader, std::string_view guid,
                    std::unique_ptr<ResourceLoad>& pending, ResourceReference& resource,
                    std::string& error)
{
    if (resource)
    {
        return SceneWorkState::ready;
    }
    if (!pending)
    {
        if (resource.acquire(&loader, guid))
        {
            return SceneWorkState::ready;
        }
        if (!loader.begin_load(guid, pending))
        {
            error = std::string(loader.last_error());
            return SceneWorkState::failed;
        }
    }
    ResourceLoadProgress status;
    const auto state = pending->status(status);
    if (state == ResourceLoadStatus::loading)
    {
        return SceneWorkState::working;
    }
    if (state != ResourceLoadStatus::ready)
    {
        error = std::string(pending->last_error());
        return SceneWorkState::failed;
    }
    ResourceHandle handle;
    if (!pending->take(handle) || !resource.adopt(&loader, handle))
    {
        (void)loader.release(handle);
        error = "Cannot retain the completed terrain resource";
        return SceneWorkState::failed;
    }
    pending.reset();
    return SceneWorkState::ready;
}

bool insert_halo(const schema::TerrainAssetDataT& source, TerrainRegionKey key, Keys& keys,
                 std::string& error)
{
    for (int z = -1; z <= 1; ++z)
    {
        for (int y = -1; y <= 1; ++y)
        {
            for (int x = -1; x <= 1; ++x)
            {
                const std::int64_t nx = std::int64_t(key.x()) + x;
                const std::int64_t ny = std::int64_t(key.y()) + y;
                const std::int64_t nz = std::int64_t(key.z()) + z;
                if (nx < INT32_MIN || nx > INT32_MAX || ny < INT32_MIN || ny > INT32_MAX ||
                    nz < INT32_MIN || nz > INT32_MAX)
                {
                    error = "Terrain halo exceeds integer coordinates";
                    return false;
                }
                const TerrainRegionKey neighbor{std::int32_t(nx), std::int32_t(ny),
                                                std::int32_t(nz), key.lod()};
                if (!intersects(source.bounds, region_bounds(source, neighbor)))
                {
                    continue;
                }
                const auto* found = entry(source, neighbor);
                if (!found || found->uniform == schema::TerrainUniformState::unknown)
                {
                    error = "Terrain requires an unknown halo region";
                    return false;
                }
                keys.insert(neighbor);
            }
        }
    }
    return true;
}
bool add_ancestors(const schema::TerrainAssetDataT& source, std::span<const TerrainRegionKey> fine,
                   Keys& required, std::string& error)
{
    for (auto key : fine)
    {
        for (std::uint8_t lod = 1; lod <= 15; ++lod)
        {
            const std::int64_t scale = std::int64_t{1} << lod;
            auto parent = [scale](std::int32_t v)
            { return static_cast<std::int32_t>(v / scale - (v % scale < 0 ? 1 : 0)); };
            TerrainRegionKey ancestor{parent(key.x()), parent(key.y()), parent(key.z()), lod};
            if (const auto* region = entry(source, ancestor))
            {
                if (region->uniform == schema::TerrainUniformState::unknown)
                {
                    error = "Terrain edit requires an unknown ancestor";
                    return false;
                }
                required.insert(ancestor);
            }
        }
    }
    return true;
}

bool collision_required(const schema::TerrainAssetDataT& source, TerrainRegionKey key,
                        const Runtime& runtime, const TransformSettings& pose)
{
    if (key.lod() != 0)
    {
        return false;
    }
    const auto bounds = region_bounds(source, key);
    for (const auto& [id, view] : runtime.views_)
    {
        (void)id;
        const auto center = local_point(view.center, pose);
        const auto radius = view.collision_radius / pose.scale.x();
        const double dx =
            std::max({bounds.minimum().x() - center.x(), center.x() - bounds.maximum().x(), 0.0});
        const double dy =
            std::max({bounds.minimum().y() - center.y(), center.y() - bounds.maximum().y(), 0.0});
        const double dz =
            std::max({bounds.minimum().z() - center.z(), center.z() - bounds.maximum().z(), 0.0});
        if (radius > 0 && std::hypot(dx, dy, dz) <= radius)
        {
            return true;
        }
    }
    for (const auto& [id, pin] : runtime.pins_)
    {
        (void)id;
        if (pin.collision_required &&
            std::find(pin.regions.begin(), pin.regions.end(), key) != pin.regions.end())
        {
            return true;
        }
    }
    return false;
}
std::uint8_t transition_mask(const schema::TerrainAssetDataT& source, TerrainRegionKey key,
                             std::span<const TerrainRegionKey> keys)
{
    const auto a = region_bounds(source, key);
    const double low[3]{a.minimum().x(), a.minimum().y(), a.minimum().z()};
    const double high[3]{a.maximum().x(), a.maximum().y(), a.maximum().z()};
    std::uint8_t mask = 0;
    for (auto other : keys)
    {
        if (other.lod() != key.lod() + 1)
        {
            continue;
        }
        const auto b = region_bounds(source, other);
        const double bl[3]{b.minimum().x(), b.minimum().y(), b.minimum().z()};
        const double bh[3]{b.maximum().x(), b.maximum().y(), b.maximum().z()};
        for (int axis = 0; axis != 3; ++axis)
        {
            const auto u = (axis + 1) % 3, v = (axis + 2) % 3;
            if (low[u] >= bh[u] || bl[u] >= high[u] || low[v] >= bh[v] || bl[v] >= high[v])
            {
                continue;
            }
            if (std::abs(low[axis] - bh[axis]) < 1e-8)
            {
                mask |= std::uint8_t(1U << (axis * 2));
            }
            if (std::abs(high[axis] - bl[axis]) < 1e-8)
            {
                mask |= std::uint8_t(1U << (axis * 2 + 1));
            }
        }
    }
    return mask;
}

bool create_output(Scene& scene, TerrainComponent owner, const TransformSettings& pose,
                   const schema::TerrainDataT& settings, const TerrainMeshOutput& mesh,
                   const schema::TerrainAssetDataT& source, ResourceLoader& resources, Node& output,
                   std::string& error)
{
    output = {};
    if (!mesh.mesh || mesh.mesh->indices.empty())
    {
        return true;
    }
    auto node = scene.create_node("Terrain region");
    TransformSettings transform;
    transform.position = global_point(mesh.local_bounds.minimum(), pose);
    transform.scale = pose.scale;
    if (!node || !node.set_transient(true) ||
        !node.get_component<Transform>().set_settings(transform))
    {
        error = "Cannot create terrain region transform";
        return false;
    }
    auto renderer = node.add_component<MeshRenderer>();
    if (!renderer || !renderer.set_transient(true) || !renderer.set_mesh_data(*mesh.mesh) ||
        !renderer.material().set_type(settings.surface_material_guid.empty()
                                          ? "builtin://materials/default-pbr"
                                          : settings.surface_material_guid) ||
        !renderer.set_rvt_role(settings.rvt_role))
    {
        error = "Cannot create terrain region renderer: " + std::string(resources.last_error());
        return false;
    }
    if (!source.surface_material.empty() &&
        (!renderer.material().set_type(source.surface_material) ||
         !renderer.material().set_v3("surfaceOrigin", mesh.local_bounds.minimum())))
    {
        error = "Cannot bind terrain surface material";
        return false;
    }
    if (!mesh.collision.parts.empty())
    {
        // A private immutable memory resource is loaded through the same injected loader.
        const auto& collision_mesh = mesh.collision.parts.front();
        static std::atomic<std::uint64_t> next{1};
        const auto sequence = next.fetch_add(1, std::memory_order_relaxed);
        char guid[37]{};
        std::snprintf(guid, sizeof(guid), "6eeec113-8ef1-4171-%04x-%012llx",
                      unsigned(sequence >> 48), (unsigned long long)(sequence & 0xffffffffffffULL));
        schema::AssetEntryT info;
        info.guid = guid;
        info.path = std::string("runtime/terrain/collision/") + guid;
        info.kind = schema::AssetKind::mesh;
        auto collision_data = *collision_mesh;
        collision_data.path = info.path;
        std::unique_ptr<Resource> candidate =
            std::make_unique<MeshResource>(std::move(info), std::move(collision_data));
        ResourceHandle handle;
        if (!resources.load_memory(candidate, {}, handle))
        {
            error =
                "Cannot prepare terrain collision resource: " + std::string(resources.last_error());
            return false;
        }
        auto collider = node.add_component<Collider>();
        const bool bound = collider && collider.set_transient(true) && collider.set_mesh(guid);
        (void)resources.release(handle);
        if (!bound)
        {
            error =
                "Cannot bind terrain collision resource: " + std::string(resources.last_error());
            return false;
        }
    }
    flecs::world(scene.world().native_world())
        .entity(node.id())
        .set<TerrainGenerated>({owner.node().id(), owner.instance_id()});
    output = node;
    return true;
}

bool build_meshes(TerrainWork& work, const TerrainMesher& mesher)
{
    TerrainReadSnapshot samples = work.input;
    samples.blocks = work.delta.blocks;
    samples.source_revision = work.delta.base_revision + 1;
    work.meshes.clear();
    for (auto key : work.keys)
    {
        if (work.cancelled->load(std::memory_order_relaxed))
        {
            return false;
        }
        TerrainMeshInput input;
        input.samples = samples;
        input.key = key;
        input.collision_required = work.collision_keys.contains(key);
        input.transition_faces = transition_mask(*samples.source, key, work.keys);
        input.cancel_requested = work.cancelled;
        TerrainMeshOutput output;
        if (!mesher.build(input, output, work.error))
        {
            return false;
        }
        work.meshes.push_back(std::move(output));
    }
    return true;
}

bool compute(TerrainWork& work, const TerrainRequest& request, bool streaming,
             const TerrainMesher& mesher)
{
    // Saved regions are streamed with the same candidate as base resources; all disk work stays
    // here.
    if (!std::holds_alternative<TerrainLoadSpec>(request) || streaming)
    {
        for (auto key : work.required)
        {
            const auto saved = work.saved_regions.find(key);
            if (saved == work.saved_regions.end() || work.input.blocks.contains(key))
            {
                continue;
            }
            auto block = std::make_shared<schema::TerrainBlockDataT>();
            if (!work.store->read_block(work.saved_slot, saved->second.payload_key, *block,
                                        work.error) ||
                block->key != key)
            {
                return false;
            }
            work.input.blocks[key] = std::move(block);
        }
    }
    work.delta = {work.input.source_revision, {}, work.input.source, work.input.blocks, {}};
    if (!streaming)
    {
        const bool success = std::visit(
            [&](const auto& spec) -> bool
            {
                using T = std::decay_t<decltype(spec)>;
                if constexpr (std::is_same_v<T, TerrainEditSpec>)
                {
                    auto edit = spec;
                    edit.expected_revision = work.input.source_revision;
                    return TerrainVolume::stage_edit(work.input, edit, work.delta, work.error);
                }
                else if constexpr (std::is_same_v<T, TerrainCollapseSpec>)
                {
                    return stage_collapse(work.input, spec, mesher, work.delta, work.fragments,
                                          work.cancelled, work.error);
                }
                else if constexpr (std::is_same_v<T, TerrainResetSpec>)
                {
                    work.delta.dirty_bounds = spec.bounds;
                    for (const auto& block : work.original_blocks)
                    {
                        work.delta.blocks[block.key] =
                            std::make_shared<const schema::TerrainBlockDataT>(block);
                        work.delta.changed.push_back(block.key);
                    }
                    return propagate_lods(work.input, work.delta, work.error);
                }
                else if constexpr (std::is_same_v<T, TerrainRetireSpec>)
                {
                    return true;
                }
                else if constexpr (std::is_same_v<T, TerrainSaveSpec>)
                {
                    for (auto& region : work.checkpoint.regions)
                    {
                        if (!region->payload_key.empty())
                        {
                            if (spec.slot != work.saved_slot)
                            {
                                schema::TerrainBlockDataT block;
                                if (!work.store->read_block(work.saved_slot, region->payload_key,
                                                            block, work.error))
                                {
                                    return false;
                                }
                                work.checkpoint_blocks.push_back(std::move(block));
                            }
                            continue;
                        }
                        const auto found = work.input.blocks.find(region->key);
                        if (found == work.input.blocks.end())
                        {
                            work.error = "Missing dirty terrain block";
                            return false;
                        }
                        flatbuffers::FlatBufferBuilder builder;
                        schema::FinishTerrainBlockDataBuffer(
                            builder, schema::TerrainBlockData::Pack(builder, found->second.get()));
                        const auto bytes =
                            std::as_bytes(std::span(builder.GetBufferPointer(), builder.GetSize()));
                        region->content_hash = region->payload_key = hash::hex(hash::fnv1a(bytes));
                        work.checkpoint_blocks.push_back(*found->second);
                    }
                    return work.store->stage(work.checkpoint, work.checkpoint_blocks, spec.slot,
                                             work.error);
                }
                else
                {
                    if (!work.store->read(spec.slot, work.checkpoint, work.error) ||
                        work.checkpoint.base_guid != work.base_guid ||
                        work.checkpoint.base_hash != work.base_hash ||
                        (!spec.expected_base_hash.empty() &&
                         spec.expected_base_hash != work.base_hash) ||
                        (!spec.checkpoint_id.empty() &&
                         spec.checkpoint_id != work.checkpoint.checkpoint_id))
                    {
                        if (work.error.empty())
                        {
                            work.error = "Terrain checkpoint identity mismatch";
                        }
                        return false;
                    }
                    // The manifest is validated as a whole; payloads stay sparse and load on
                    // demand.
                    for (const auto& region : work.checkpoint.regions)
                    {
                        if (!entry(*work.input.source, region->key))
                        {
                            work.error = "Checkpoint references an unknown terrain region";
                            return false;
                        }
                        if (std::find(work.required.begin(), work.required.end(), region->key) ==
                            work.required.end())
                        {
                            continue;
                        }
                        schema::TerrainBlockDataT block;
                        if (!work.store->read_block(spec.slot, region->payload_key, block,
                                                    work.error) ||
                            block.key != region->key)
                        {
                            return false;
                        }
                        work.delta.blocks[block.key] =
                            std::make_shared<const schema::TerrainBlockDataT>(std::move(block));
                        work.delta.changed.push_back(region->key);
                    }
                    if (!work.checkpoint.debris.empty())
                    {
                        return restore_fragments(work.input, work.checkpoint, *work.store,
                                                 spec.slot, mesher, work.fragments, work.error);
                    }
                    return true;
                }
            },
            request);
        if (!success)
        {
            return false;
        }
    }
    return std::holds_alternative<TerrainSaveSpec>(request) && !streaming
               ? true
               : build_meshes(work, mesher);
}
} // namespace

TerrainOperation::~TerrainOperation() noexcept
{
    if (work)
    {
        work->cancelled->store(true, std::memory_order_relaxed);
    }
    geometry.reset();
    if (scope_world && new_scope.value)
    {
        scope_world->release_scope(new_scope);
    }
}

TerrainSystem::TerrainSystem(WorldSystem& world, ResourceLoader& resources, TaskSystem& tasks,
                             RenderSystem& renderer, PhysicsSystem& physics,
                             std::unique_ptr<TerrainRegionPlanner> planner,
                             std::unique_ptr<TerrainMesher> mesher) noexcept
    : world_(world), resources_(resources), tasks_(tasks), renderer_(renderer), physics_(physics),
      planner_(std::move(planner)), mesher_(std::move(mesher))
{
}
TerrainSystem::~TerrainSystem() noexcept
{
    shutdown();
}
bool TerrainSystem::on_initialize(const EngineConfig&)
{
    if (!planner_ || !mesher_ || !tasks_.is_initialized() || !world_.native_world())
    {
        return false;
    }
    auto& registry = world_.component_registry();
    if (!registry.contains(TerrainComponent::component_type) && !registry.add<TerrainComponent>())
    {
        return false;
    }
    flecs::world(world_.native_world()).component<TerrainGenerated>();
    return true;
}

bool TerrainSystem::request(TerrainComponent component, TerrainRequest request,
                            TerrainOperationId& output)
{
    output = {};
    auto* state = component.runtime();
    TerrainComponent::Data settings;
    if (!is_initialized() || !component || component.node().world() != &world_ ||
        world_.find_node(component.node().id()) != component.node() ||
        !component.get_settings(settings) || !state->ready_ || !component.enabled())
    {
        return false;
    }
    const auto budget = settings.budget ? *settings.budget : schema::TerrainBudgetDataT{};
    if (state->operations_.size() >= budget.max_requests || state->next_operation_ == UINT64_MAX)
    {
        return false;
    }
    TransformSettings pose;
    if (!pose_of(component, pose))
    {
        return false;
    }
    const bool valid = std::visit(
        [&](auto& spec) -> bool
        {
            using T = std::decay_t<decltype(spec)>;
            if constexpr (std::is_same_v<T, TerrainEditSpec>)
            {
                if (spec.brushes.empty() || spec.brushes.size() > 64)
                {
                    return false;
                }
                for (auto& brush : spec.brushes)
                {
                    if (!finite(brush.center) || !finite(brush.extent) || brush.extent.x() <= 0 ||
                        brush.extent.y() <= 0 || brush.extent.z() <= 0)
                    {
                        return false;
                    }
                    brush.center = local_point(brush.center, pose);
                    brush.extent = {brush.extent.x() / pose.scale.x(),
                                    brush.extent.y() / pose.scale.x(),
                                    brush.extent.z() / pose.scale.x()};
                }
                return true;
            }
            else if constexpr (std::is_same_v<T, TerrainCollapseSpec>)
            {
                spec.bounds = local_bounds(spec.bounds, pose);
                return valid_bounds(spec.bounds) && spec.min_samples > 0 &&
                       spec.fragment_cells > 0 && spec.fragment_cells <= 8 &&
                       spec.max_fragments > 0 && spec.max_fragments <= 128 &&
                       std::isfinite(spec.density) && spec.density > 0 &&
                       spec.connectivity_block_limit > 0 && spec.connectivity_block_limit <= 256;
            }
            else if constexpr (std::is_same_v<T, TerrainResetSpec>)
            {
                spec.bounds = local_bounds(spec.bounds, pose);
                return valid_bounds(spec.bounds);
            }
            else if constexpr (std::is_same_v<T, TerrainRetireSpec>)
            {
                return spec.max_count > 0 && spec.max_count <= 128 &&
                       std::isfinite(spec.max_age_seconds) && spec.max_age_seconds >= 0;
            }
            else
            {
                return !spec.slot.empty() && spec.slot.size() <= 96 &&
                       spec.checkpoint_id.size() <= 96;
            }
        },
        request);
    if (!valid)
    {
        return false;
    }
    auto operation = std::make_unique<TerrainOperation>();
    operation->request = std::move(request);
    operation->snapshot.id = {world_.epoch(), component.node().id(), component.instance_id(),
                              state->next_operation_++};
    operation->settings_revision = state->settings_revision_;
    operation->work = std::make_shared<TerrainWork>();
    operation->pose = pose;
    output = operation->snapshot.id;
    state->operations_.emplace(output.sequence, std::move(operation));
    return true;
}

bool TerrainSystem::update(TerrainComponent component, SceneScopeId scope,
                           const SceneWorkBudget& frame_budget)
{
    auto* state = component.runtime();
    state->system_ = this;
    TerrainComponent::Data settings;
    if (!component.get_settings(settings))
    {
        return false;
    }
    const bool enabled =
        component.enabled() && component.node().active() && !settings.terrain_guid.empty();
    if (!enabled)
    {
        state->streaming_.reset();
        for (auto& [id, operation] : state->operations_)
        {
            (void)id;
            if (!terminal(operation->snapshot.state) && !operation->persistence_commit)
            {
                operation->work->cancelled->store(true, std::memory_order_relaxed);
                fail(*operation, "cancelled", "Terrain owner is disabled");
                operation->snapshot.state = TerrainOperationState::cancelled;
            }
            if (operation->future.valid() &&
                operation->future.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
            {
                const bool success = operation->future.get();
                if (operation->persistence_commit)
                {
                    operation->snapshot.state =
                        success ? TerrainOperationState::completed : TerrainOperationState::failed;
                    operation->snapshot.checkpoint_id =
                        success ? operation->work->checkpoint.checkpoint_id : "";
                    operation->snapshot.error = success ? "" : operation->work->error;
                    operation->snapshot.error_code = success ? "" : "persistence_failed";
                    progress(*operation, "manifest", success ? 1 : 0, 1,
                             success ? SceneWorkState::ready : SceneWorkState::failed);
                }
            }
        }
    }
    if (state->generated_enabled_ != enabled)
    {
        std::vector<Node> generated;
        for (const auto& [key, region] : state->regions_)
        {
            (void)key;
            if (region.node)
            {
                generated.push_back(region.node);
            }
        }
        for (const auto& fragment : state->fragments_)
        {
            if (fragment.node)
            {
                generated.push_back(fragment.node);
            }
        }
        for (auto node : generated)
        {
            (void)node.set_enabled(enabled);
        }
        state = component.runtime();
        state->generated_enabled_ = enabled;
        state->plan_dirty_ = enabled;
    }
    std::erase_if(state->operations_,
                  [](const auto& item)
                  {
                      return item.second->discard_when_terminal &&
                             terminal(item.second->snapshot.state) && !item.second->future.valid();
                  });
    if (!enabled)
    {
        state->ready_ = settings.terrain_guid.empty();
        return true;
    }
    TransformSettings pose;
    if (!pose_of(component, pose))
    {
        state->error_ = "Terrain requires a root with translation and positive uniform scale";
        return false;
    }
    const auto budget = settings.budget ? *settings.budget : schema::TerrainBudgetDataT{};
    if (state->ready_ && state->active_pose_ != pose &&
        state->prepared_settings_revision_ == state->settings_revision_)
    {
        ++state->settings_revision_;
    }
    if (state->streaming_ &&
        (state->streaming_->settings_revision != state->settings_revision_ ||
         state->streaming_->demand_revision != state->demand_revision_) &&
        terminal(state->streaming_->snapshot.state))
    {
        state->streaming_.reset();
    }
    const bool rebuilding = state->prepared_settings_revision_ != state->settings_revision_;
    if (rebuilding && !state->streaming_)
    {
        if (state->loading_settings_revision_ != state->settings_revision_)
        {
            state->source_loading_.reset();
            state->material_loading_.reset();
            state->pending_source_.reset();
            state->pending_material_.reset();
            state->loading_settings_revision_ = state->settings_revision_;
        }
        const auto source = load(resources_, settings.terrain_guid, state->source_loading_,
                                 state->pending_source_, state->error_);
        const auto material =
            load(resources_,
                 settings.surface_material_guid.empty() ? "builtin://materials/default-pbr"
                                                        : settings.surface_material_guid,
                 state->material_loading_, state->pending_material_, state->error_);
        if (source == SceneWorkState::failed || material == SceneWorkState::failed)
        {
            return false;
        }
        if (source != SceneWorkState::ready || material != SceneWorkState::ready)
        {
            return true;
        }
        const auto* data = state->pending_source_.data<TerrainResource>();
        if (!data)
        {
            state->error_ = "Terrain source has the wrong type";
            return false;
        }
        auto operation = std::make_unique<TerrainOperation>();
        operation->work = std::make_shared<TerrainWork>();
        operation->replace_source =
            !state->source_ || state->source_->info().guid != state->pending_source_->info().guid ||
            state->source_->info().content_hash != state->pending_source_->info().content_hash;
        if (operation->replace_source &&
            !operation->candidate_volume.initialize(*data, state->error_))
        {
            return false;
        }
        operation->reconfigure = true;
        operation->residency_only = true;
        operation->initial = scope.value != 0;
        operation->settings_revision = state->settings_revision_;
        operation->demand_revision = state->demand_revision_;
        operation->pose = pose;
        if (state->views_.empty())
        {
            state->views_[0] = settings.initial_view ? *settings.initial_view : TerrainView{};
        }
        state->streaming_ = std::move(operation);
    }
    TerrainOperation* operation = state->streaming_.get();
    if (!operation)
    {
        for (auto& [id, candidate] : state->operations_)
        {
            (void)id;
            if (!terminal(candidate->snapshot.state))
            {
                operation = candidate.get();
                break;
            }
        }
    }
    if (!operation && state->plan_dirty_ && settings.streaming_enabled)
    {
        auto streaming = std::make_unique<TerrainOperation>();
        streaming->residency_only = true;
        streaming->work = std::make_shared<TerrainWork>();
        streaming->settings_revision = state->settings_revision_;
        streaming->demand_revision = state->demand_revision_;
        streaming->pose = pose;
        state->streaming_ = std::move(streaming);
        operation = state->streaming_.get();
    }
    if (!operation)
    {
        return true;
    }
    if (terminal(operation->snapshot.state))
    {
        state->error_ = operation->snapshot.error;
        return false;
    }
    auto& op = *operation;
    auto& work = *op.work;
    if (op.settings_revision != state->settings_revision_ || op.pose != pose ||
        (op.residency_only && op.demand_revision != state->demand_revision_))
    {
        op.cancelled = true;
    }
    if (op.cancelled)
    {
        work.cancelled->store(true, std::memory_order_relaxed);
        if (op.future.valid() &&
            op.future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        {
            return true;
        }
        if (op.future.valid())
        {
            (void)op.future.get();
        }
        if (op.geometry)
        {
            (void)op.geometry->cancel();
        }
        fail(op, "cancelled", "Terrain operation was cancelled before publication");
        op.snapshot.state = TerrainOperationState::cancelled;
        if (state->streaming_.get() == &op)
        {
            state->streaming_.reset();
        }
        return true;
    }
    auto& volume = op.replace_source ? op.candidate_volume : state->volume_;
    if (op.phase == 0)
    {
        std::uint64_t expected = std::visit(
            [](const auto& spec) -> std::uint64_t
            {
                if constexpr (requires { spec.expected_revision; })
                {
                    return spec.expected_revision;
                }
                return 0;
            },
            op.request);
        if (!op.residency_only && expected && expected != state->revision_)
        {
            fail(op, "revision_conflict", "Terrain content revision changed before execution");
            return true;
        }
        TerrainDelta capture;
        if (!volume.stage_regions({}, {}, capture, work.error))
        {
            fail(op, "capture_failed", work.error);
            return true;
        }
        work.input = {capture.base_revision, capture.source, capture.blocks, {}};
        const bool loading_checkpoint =
            !op.residency_only && std::holds_alternative<TerrainLoadSpec>(op.request);
        if (loading_checkpoint)
        {
            work.input.blocks.clear();
        }
        if (!op.replace_source && !loading_checkpoint)
        {
            work.saved_regions = state->saved_regions_;
            work.saved_slot = state->saved_slot_;
        }
        work.store = std::make_shared<TerrainStateStore>(App::persistent_data_path() / "terrain");
        work.base_guid =
            op.replace_source ? state->pending_source_->info().guid : state->source_->info().guid;
        work.base_hash = op.replace_source ? state->pending_source_->info().content_hash
                                           : state->source_->info().content_hash;
        op.snapshot.base_revision = state->revision_;
        Keys required;
        if (op.residency_only)
        {
            TerrainPlanInput input;
            input.source = work.input.source;
            input.budget = budget;
            if (settings.streaming_enabled)
            {
                for (const auto& [id, view] : state->views_)
                {
                    (void)id;
                    auto local = view;
                    local.center = local_point(view.center, pose);
                    local.visual_radius /= pose.scale.x();
                    local.collision_radius /= pose.scale.x();
                    input.views.push_back(local);
                }
                for (const auto& [id, pin] : state->pins_)
                {
                    (void)id;
                    input.pins.push_back(pin);
                }
            }
            if (!op.replace_source)
            {
                for (const auto& [key, region] : state->regions_)
                {
                    auto status = region.status;
                    status.dirty = state->dirty_.contains(key);
                    input.resident.push_back(std::move(status));
                }
            }
            TerrainRegionPlan plan;
            if (!planner_->plan(input, plan, work.error))
            {
                fail(op, "planning_failed", work.error);
            }
            else
            {
                work.keys = plan.retain;
                work.keys.insert(work.keys.end(), plan.acquire.begin(), plan.acquire.end());
                op.retired = std::move(plan.retire);
                if (op.replace_source)
                {
                    for (const auto& [key, region] : state->regions_)
                    {
                        (void)region;
                        op.retired.push_back(key);
                    }
                }
                for (auto key : work.keys)
                {
                    if (!insert_halo(*work.input.source, key, required, work.error))
                    {
                        fail(op, "unknown_region", work.error);
                        break;
                    }
                }
            }
        }
        else
        {
            for (const auto& [key, region] : state->regions_)
            {
                (void)region;
                work.keys.push_back(key);
            }
            std::vector<Bounds3> bounds;
            if (const auto* edit = std::get_if<TerrainEditSpec>(&op.request))
            {
                for (const auto& brush : edit->brushes)
                {
                    bounds.push_back(
                        {{brush.center.x() - brush.extent.x(), brush.center.y() - brush.extent.y(),
                          brush.center.z() - brush.extent.z()},
                         {brush.center.x() + brush.extent.x(), brush.center.y() + brush.extent.y(),
                          brush.center.z() + brush.extent.z()}});
                }
            }
            if (const auto* collapse = std::get_if<TerrainCollapseSpec>(&op.request))
            {
                bounds.push_back(collapse->bounds);
            }
            if (const auto* reset = std::get_if<TerrainResetSpec>(&op.request))
            {
                bounds.push_back(reset->bounds);
            }
            for (const auto& bound : bounds)
            {
                std::vector<TerrainRegionKey> affected;
                if (!enumerate(*work.input.source, bound, 0, 256, affected))
                {
                    fail(op, "region_budget", "Terrain operation exceeds its region limit");
                    break;
                }
                work.input.regions.insert(work.input.regions.end(), affected.begin(),
                                          affected.end());
            }
            Keys affected(work.input.regions.begin(), work.input.regions.end());
            work.input.regions.assign(affected.begin(), affected.end());
            for (auto key : work.input.regions)
            {
                if (!insert_halo(*work.input.source, key, required, work.error))
                {
                    fail(op, "unknown_region", work.error);
                    break;
                }
            }
            if (!add_ancestors(*work.input.source, work.input.regions, required, work.error))
            {
                fail(op, "unknown_region", work.error);
            }
            for (auto key : work.keys)
            {
                if (!insert_halo(*work.input.source, key, required, work.error))
                {
                    fail(op, "unknown_region", work.error);
                    break;
                }
            }
        }
        if (terminal(op.snapshot.state))
        {
            return true;
        }
        if (required.size() > budget.max_regions * 8ULL || work.keys.size() > 256)
        {
            fail(op, "region_budget",
                 "Terrain candidate and its halo exceed the configured region budget");
            return true;
        }
        op.required.assign(required.begin(), required.end());
        work.required = op.required;
        for (auto key : work.keys)
        {
            if (settings.collision_enabled &&
                collision_required(*work.input.source, key, *state, pose))
            {
                work.collision_keys.insert(key);
            }
        }
        op.phase = 1;
        op.snapshot.state = TerrainOperationState::waiting;
    }
    if (op.phase == 1)
    {
        std::size_t complete = 0, started = 0;
        for (const auto& [key, pending] : op.loads)
        {
            (void)key;
            if (pending.request)
            {
                ++started;
            }
        }
        const bool reset =
            std::holds_alternative<TerrainResetSpec>(op.request) && !op.residency_only;
        for (auto key : op.required)
        {
            const auto* region = entry(*work.input.source, key);
            if ((work.saved_regions.contains(key) &&
                 (!reset || std::find(work.input.regions.begin(), work.input.regions.end(), key) ==
                                work.input.regions.end())) ||
                region->uniform != schema::TerrainUniformState::mixed ||
                (work.input.blocks.contains(key) && !reset))
            {
                ++complete;
                continue;
            }
            auto& pending = op.loads[key];
            if (!pending.request && !pending.resource)
            {
                if (started >= budget.max_tasks)
                {
                    continue;
                }
                ++started;
            }
            const auto status =
                load(resources_, region->payload, pending.request, pending.resource, work.error);
            if (status == SceneWorkState::failed)
            {
                fail(op, "resource_failed", work.error);
                return true;
            }
            if (status != SceneWorkState::ready)
            {
                continue;
            }
            const auto* block = pending.resource.data<TerrainBlockResource>();
            if (!block || block->key != key ||
                pending.resource->info().content_hash != region->content_hash)
            {
                fail(op, "payload_mismatch",
                     "Terrain block identity or content hash does not match its index");
                return true;
            }
            if (reset && std::find(work.input.regions.begin(), work.input.regions.end(), key) !=
                             work.input.regions.end())
            {
                if (std::none_of(work.original_blocks.begin(), work.original_blocks.end(),
                                 [&](const auto& value) { return value.key == key; }))
                {
                    work.original_blocks.push_back(*block);
                }
            }
            else if (!work.input.blocks.contains(key))
            {
                work.input.blocks[key] = std::make_shared<const schema::TerrainBlockDataT>(*block);
            }
            ++complete;
        }
        progress(op, "residency", complete, op.required.size(), SceneWorkState::working);
        if (complete != op.required.size())
        {
            return true;
        }
        const std::uint64_t sample_bytes =
            std::uint64_t(work.input.source->block_cells) * work.input.source->block_cells *
            work.input.source->block_cells * (sizeof(float) + sizeof(schema::TerrainLayerSample));
        if (work.input.blocks.size() * sample_bytes * 2 > budget.cpu_bytes)
        {
            fail(op, "cpu_budget",
                 "Terrain active and candidate sample data exceed the CPU budget");
            return true;
        }
        if (reset)
        {
            for (auto key : work.input.regions)
            {
                const auto* region = entry(*work.input.source, key);
                if (region->uniform == schema::TerrainUniformState::mixed)
                {
                    continue;
                }
                auto original = work.input;
                original.blocks.erase(key);
                schema::TerrainBlockDataT block;
                if (!materialize_block(original, key, block))
                {
                    fail(op, "reset_failed", "Cannot restore a uniform terrain region");
                    return true;
                }
                work.original_blocks.push_back(std::move(block));
            }
        }
        if (const auto* retire = std::get_if<TerrainRetireSpec>(&op.request);
            retire && !op.residency_only)
        {
            for (const auto& fragment : state->fragments_)
            {
                if (op.retire_fragments.size() == retire->max_count)
                {
                    break;
                }
                if (retire->max_age_seconds == 0 ||
                    simulation_time_ - fragment.published_at >= retire->max_age_seconds)
                {
                    op.retire_fragments.push_back(fragment.stable_id);
                }
            }
        }
        if (reset || (std::holds_alternative<TerrainLoadSpec>(op.request) && !op.residency_only))
        {
            for (const auto& fragment : state->fragments_)
            {
                const auto included = std::count_if(
                    fragment.source_regions.begin(), fragment.source_regions.end(),
                    [&](auto key)
                    {
                        return std::find(work.input.regions.begin(), work.input.regions.end(),
                                         key) != work.input.regions.end();
                    });
                if (reset && included != 0 &&
                    included != static_cast<std::ptrdiff_t>(fragment.source_regions.size()))
                {
                    fail(op, "partial_fragment",
                         "Reset bounds must include every source region of intersecting debris");
                    return true;
                }
                if (!reset || included != 0)
                {
                    op.retire_fragments.push_back(fragment.stable_id);
                }
            }
        }
        if (const auto* save = std::get_if<TerrainSaveSpec>(&op.request);
            save && !op.residency_only)
        {
            work.checkpoint.base_guid = work.base_guid;
            work.checkpoint.base_hash = work.base_hash;
            work.checkpoint.checkpoint_id = save->checkpoint_id;
            work.checkpoint.revision = state->revision_;
            work.checkpoint.next_debris_id = state->next_debris_;
            work.checkpoint.retired_ids = state->retired_ids_;
            for (const auto& [key, saved] : state->saved_regions_)
            {
                if (!state->dirty_.contains(key))
                {
                    work.checkpoint.regions.push_back(
                        std::make_unique<schema::TerrainRegionStateT>(saved));
                }
            }
            for (auto key : state->dirty_)
            {
                auto region = std::make_unique<schema::TerrainRegionStateT>();
                region->key = key;
                work.checkpoint.regions.push_back(std::move(region));
            }
            if (!capture_fragments(state->fragments_, simulation_time_, work.checkpoint,
                                   work.checkpoint_blocks, work.error))
            {
                fail(op, "capture_failed", work.error);
                return true;
            }
            op.snapshot.captured_revision = state->revision_;
        }
        if (!work.store && (std::holds_alternative<TerrainSaveSpec>(op.request) ||
                            std::holds_alternative<TerrainLoadSpec>(op.request)))
        {
            work.store =
                std::make_shared<TerrainStateStore>(App::persistent_data_path() / "terrain");
        }
        auto shared = op.work;
        const auto request = op.request;
        auto mesher = mesher_;
        const bool streaming = op.residency_only;
        op.future = tasks_.submit("Terrain.prepare", [shared, request, mesher, streaming]()
                                  { return compute(*shared, request, streaming, *mesher); });
        if (!op.future.valid())
        {
            return true;
        }
        op.phase = 2;
        op.snapshot.state = TerrainOperationState::preparing;
        const bool saving =
            !op.residency_only && std::holds_alternative<TerrainSaveSpec>(op.request);
        progress(op, saving ? "checkpoint" : "meshing", 0,
                 saving ? work.checkpoint.regions.size() + work.checkpoint.debris.size()
                        : work.keys.size(),
                 SceneWorkState::working);
        return true;
    }
    if (op.phase == 2)
    {
        if (op.future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        {
            return true;
        }
        if (!op.future.get())
        {
            fail(op, "preparation_failed", work.error);
            return true;
        }
        if (std::holds_alternative<TerrainSaveSpec>(op.request) && !op.residency_only)
        {
            auto shared = op.work;
            op.future = tasks_.submit("Terrain.save_manifest",
                                      [shared]() { return shared->store->publish(shared->error); });
            if (!op.future.valid())
            {
                fail(op, "task_budget", "Cannot submit terrain manifest publication");
                return true;
            }
            progress(op, "manifest", 0, 1, SceneWorkState::working);
            op.persistence_commit = true;
            op.phase = 5;
            return true;
        }
        if (const auto* collapse = std::get_if<TerrainCollapseSpec>(&op.request);
            collapse && !op.residency_only)
        {
            for (auto& fragment : work.fragments)
            {
                fragment.density = collapse->density;
            }
        }
        if (op.replace_source)
        {
            for (const auto& fragment : state->fragments_)
            {
                op.retire_fragments.push_back(fragment.stable_id);
            }
        }
        std::uint64_t bytes = 0, collision_bytes = 0;
        for (const auto& mesh : work.meshes)
        {
            if (mesh.mesh)
            {
                bytes += mesh.mesh->vertices.size() * sizeof(schema::MeshVertex) +
                         mesh.mesh->indices.size() * sizeof(std::uint32_t);
            }
            for (const auto& part : mesh.collision.parts)
            {
                if (part)
                {
                    collision_bytes += part->vertices.size() * sizeof(schema::MeshVertex) +
                                       part->indices.size() * sizeof(std::uint32_t);
                }
            }
        }
        if (bytes * 2 > budget.gpu_bytes || collision_bytes * 2 > budget.collision_bytes)
        {
            fail(op, "geometry_budget",
                 "Terrain old and candidate geometry exceed configured budgets");
            return true;
        }
        if (!op.initial)
        {
            if (!world_.begin_scope(SceneLoadKind::append_batch, component.node().scene()->id(),
                                    op.new_scope))
            {
                fail(op, "scope_failed", "Cannot create terrain geometry candidate");
                return true;
            }
            op.scope_world = &world_;
            op.geometry_spec.world_epoch = world_.epoch();
            op.geometry_spec.scene_id = component.node().scene()->id();
            op.geometry_spec.new_nodes_scope = op.new_scope;
            for (auto key : op.retired)
            {
                const auto found = state->regions_.find(key);
                if (found != state->regions_.end() && found->second.node)
                {
                    op.geometry_spec.retired_nodes.push_back(found->second.node);
                }
            }
            for (const auto& fragment : state->fragments_)
            {
                if (std::find(op.retire_fragments.begin(), op.retire_fragments.end(),
                              fragment.stable_id) != op.retire_fragments.end())
                {
                    op.geometry_spec.retired_nodes.push_back(fragment.node);
                }
            }
        }
        op.phase = 3;
    }
    if (op.phase == 3)
    {
        std::size_t done = 0;
        while (op.next_mesh < work.meshes.size() && done++ < std::max(1U, frame_budget.items))
        {
            const auto& mesh = work.meshes[op.next_mesh];
            const auto previous = state->regions_.find(mesh.key);
            const bool reuse = !op.initial && !op.reconfigure &&
                               previous != state->regions_.end() && previous->second.node;
            if (reuse)
            {
                const auto node = previous->second.node;
                auto renderer = node.get_component<MeshRenderer>();
                auto collider = node.get_component<Collider>();
                const bool empty = !mesh.mesh || mesh.mesh->indices.empty();
                // Changing collider presence creates a fully prepared replacement node.
                if (bool(collider) == !mesh.collision.parts.empty())
                {
                    op.geometry_spec.render_changes.push_back(
                        {renderer, renderer.instance_id(), empty, mesh.mesh});
                    if (collider)
                    {
                        schema::ColliderDataT data;
                        data.shape = schema::ColliderShape::mesh;
                        op.geometry_spec.collision_changes.push_back(
                            {collider, collider.instance_id(), empty, std::move(data),
                             mesh.collision});
                    }
                    op.prepared_nodes[mesh.key] = node;
                    ++op.next_mesh;
                    continue;
                }
                op.geometry_spec.retired_nodes.push_back(node);
            }
            if (!reuse && previous != state->regions_.end() && previous->second.node &&
                std::find(op.geometry_spec.retired_nodes.begin(),
                          op.geometry_spec.retired_nodes.end(),
                          previous->second.node) == op.geometry_spec.retired_nodes.end())
            {
                op.geometry_spec.retired_nodes.push_back(previous->second.node);
            }
            Node created;
            const auto create = [&](Scene& scene)
            {
                return create_output(scene, component, pose, settings, mesh, *work.input.source,
                                     resources_, created, work.error);
            };
            const bool ok = op.initial ? world_.with_scope_write(scope, create)
                                       : world_.with_scope_write(op.new_scope, create);
            if (!ok)
            {
                fail(op, "node_failed",
                     work.error.empty() ? "Cannot access the terrain candidate scope" : work.error);
                return true;
            }
            state = component.runtime();
            op.prepared_nodes[mesh.key] = created;
            ++op.next_mesh;
        }
        if (op.next_mesh != work.meshes.size())
        {
            return true;
        }
        if (!work.fragments.empty() && op.prepared_fragments.empty())
        {
            const auto create = [&](Scene& scene)
            {
                return create_fragments(scene, component, pose, settings, work.fragments,
                                        op.prepared_fragments, work.error);
            };
            if (!world_.with_scope_write(op.new_scope, create))
            {
                fail(op, "fragment_failed", work.error);
                return true;
            }
        }
        state = component.runtime();
        if (op.initial)
        {
            op.phase = 4;
        }
        else
        {
            op.geometry = std::make_unique<GeometryUpdate>(world_, renderer_, physics_, tasks_);
            if (!op.geometry->begin(op.geometry_spec))
            {
                fail(op, "geometry_failed", "Cannot begin terrain geometry publication");
                return true;
            }
            op.phase = 4;
        }
    }
    if (op.phase == 4)
    {
        if (!op.initial)
        {
            auto limited = frame_budget;
            limited.upload_bytes = std::min(limited.upload_bytes, budget.upload_bytes_per_frame);
            const auto result = op.geometry->prepare(limited);
            GeometryUpdateSnapshot status;
            if (op.geometry->status(status))
            {
                op.snapshot.stages = std::move(status.stages);
            }
            if (result == SceneWorkState::failed)
            {
                fail(op, "geometry_failed", status.error_code);
                return true;
            }
            if (result != SceneWorkState::ready)
            {
                return true;
            }
        }
        op.snapshot.state = TerrainOperationState::ready;
        if (!op.initial && world_.paused())
        {
            return true;
        }
        state = component.runtime();
        if (!(op.replace_source ? op.candidate_volume : state->volume_).validate_delta(work.delta))
        {
            fail(op, "revision_conflict", "Terrain samples changed before publication");
            return true;
        }
        // Reserve all owned containers before the geometry commit.
        auto regions = op.replace_source
                           ? std::map<TerrainRegionKey, Runtime::Region, TerrainRegionLess>{}
                           : state->regions_;
        for (auto key : op.retired)
        {
            regions.erase(key);
        }
        const auto revision = state->revision_ + 1;
        for (auto key : work.keys)
        {
            Runtime::Region value;
            value.node = op.prepared_nodes[key];
            value.status.key = key;
            value.status.known = true;
            value.status.state = SceneWorkState::ready;
            value.status.density_revision = value.status.mesh_revision = revision;
            value.status.collision_required = work.collision_keys.contains(key);
            value.status.collision_revision = value.status.collision_required ? revision : 0;
            regions[key] = std::move(value);
        }
        auto dirty = op.replace_source ? Keys{} : state->dirty_;
        if (!op.residency_only)
        {
            dirty.insert(work.delta.changed.begin(), work.delta.changed.end());
        }
        if (std::holds_alternative<TerrainResetSpec>(op.request) && !op.residency_only)
        {
            for (const auto& block : work.original_blocks)
            {
                dirty.erase(block.key);
            }
        }
        const bool loaded =
            !op.residency_only && std::holds_alternative<TerrainLoadSpec>(op.request);
        if (loaded)
        {
            dirty.clear();
        }
        auto saved_regions = (op.replace_source || loaded) ? decltype(state->saved_regions_){}
                                                           : state->saved_regions_;
        if (loaded)
        {
            for (const auto& region : work.checkpoint.regions)
            {
                saved_regions[region->key] = *region;
            }
        }
        if (!op.residency_only && std::holds_alternative<TerrainResetSpec>(op.request))
        {
            for (const auto& block : work.original_blocks)
            {
                saved_regions.erase(block.key);
            }
        }
        Keys keep(op.required.begin(), op.required.end());
        keep.insert(dirty.begin(), dirty.end());
        std::erase_if(work.delta.blocks,
                      [&](const auto& item) { return !keep.contains(item.first); });
        auto fragments = op.replace_source ? std::vector<Runtime::Fragment>{} : state->fragments_;
        auto retired_ids = loaded              ? work.checkpoint.retired_ids
                           : op.replace_source ? std::vector<std::uint64_t>{}
                                               : state->retired_ids_;
        std::erase_if(fragments,
                      [&](const auto& fragment)
                      {
                          return std::find(op.retire_fragments.begin(), op.retire_fragments.end(),
                                           fragment.stable_id) != op.retire_fragments.end();
                      });
        if (!loaded && !op.replace_source)
        {
            retired_ids.insert(retired_ids.end(), op.retire_fragments.begin(),
                               op.retire_fragments.end());
        }
        auto next_debris = loaded              ? work.checkpoint.next_debris_id
                           : op.replace_source ? 1
                                               : state->next_debris_;
        for (std::size_t i = 0; i < work.fragments.size(); ++i)
        {
            Runtime::Fragment fragment;
            fragment.stable_id =
                work.fragments[i].stable_id ? work.fragments[i].stable_id : next_debris++;
            fragment.source_region = work.fragments[i].source_region;
            fragment.node = op.prepared_fragments[i];
            fragment.published_at = simulation_time_ - work.fragments[i].age;
            fragment.samples = work.fragments[i].samples;
            fragment.density = work.fragments[i].density;
            fragment.source_regions = work.fragments[i].source_regions;
            fragment.bounds = work.fragments[i].bounds;
            fragments.push_back(std::move(fragment));
        }
        GeometryPublication published;
        if (!op.initial && !op.geometry->publish(published))
        {
            fail(op, "publication_failed", "Terrain geometry preflight failed");
            return true;
        }
        state = component.runtime();
        (op.replace_source ? op.candidate_volume : state->volume_).publish(std::move(work.delta));
        if (op.replace_source)
        {
            state->volume_ = std::move(op.candidate_volume);
        }
        if (op.reconfigure)
        {
            state->source_ = std::move(state->pending_source_);
            state->material_ = std::move(state->pending_material_);
            state->active_settings_ = settings;
            state->prepared_settings_revision_ = op.settings_revision;
        }
        state->regions_.swap(regions);
        state->dirty_.swap(dirty);
        state->saved_regions_.swap(saved_regions);
        if (loaded)
        {
            state->saved_slot_ = std::get<TerrainLoadSpec>(op.request).slot;
        }
        else if (op.replace_source)
        {
            state->saved_slot_.clear();
        }
        state->active_pose_ = pose;
        state->fragments_.swap(fragments);
        state->retired_ids_.swap(retired_ids);
        state->next_debris_ = next_debris;
        state->revision_ = revision;
        state->ready_ = true;
        state->plan_dirty_ = false;
        state->error_.clear();
        op.snapshot.published_revision = revision;
        op.snapshot.content_revision = published.content_revision;
        op.snapshot.dirty_bounds = published.dirty_bounds;
        op.snapshot.created_fragments = static_cast<std::uint32_t>(work.fragments.size());
        op.snapshot.retired_fragments = static_cast<std::uint32_t>(op.retire_fragments.size());
        op.snapshot.state = TerrainOperationState::completed;
        progress(op, "publication", 1, 1, SceneWorkState::ready);
        if (op.geometry)
        {
            op.geometry->release();
            op.geometry.reset();
        }
        op.new_scope = {};
        op.loads.clear();
    }
    if (op.phase == 5)
    {
        if (op.future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        {
            return true;
        }
        if (!op.future.get())
        {
            fail(op, "persistence_failed", work.error);
            return true;
        }
        state->saved_regions_.clear();
        for (const auto& region : work.checkpoint.regions)
        {
            state->saved_regions_[region->key] = *region;
        }
        state->saved_slot_ = std::get<TerrainSaveSpec>(op.request).slot;
        state->dirty_.clear();
        state->plan_dirty_ = true;
        op.snapshot.checkpoint_id = work.checkpoint.checkpoint_id;
        op.snapshot.state = TerrainOperationState::completed;
        progress(op, "manifest", 1, 1, SceneWorkState::ready);
    }
    if (state->streaming_.get() == &op && terminal(op.snapshot.state))
    {
        const bool success = op.snapshot.state == TerrainOperationState::completed;
        if (!success)
        {
            state->error_ = op.snapshot.error;
        }
        state->streaming_.reset();
        return success;
    }
    return true;
}

SceneWorkState TerrainSystem::prepare_scope(SceneScopeId scope, const SceneWorkBudget& budget)
{
    auto* scene = scope == SceneScopeId{} ? world_.scene() : world_.scope_candidate(scope);
    auto& status = scopes_[scope];
    status.scope = scope;
    status.stages.clear();
    const auto invalid = [&](std::string message)
    {
        status.state = SceneWorkState::failed;
        SceneWorkProgress progress;
        progress.domain = "terrain";
        progress.stage = "initial_regions";
        progress.state = status.state;
        progress.error_code = "invalid_scope";
        progress.error = std::move(message);
        status.stages.push_back(std::move(progress));
        return status.state;
    };
    if (!scene)
    {
        return invalid("Terrain preparation has no scene");
    }
    std::vector<Node> nodes;
    if (!scene->collect_nodes(nodes))
    {
        return invalid("Terrain preparation cannot enumerate scene nodes");
    }
    status.stages.clear();
    status.state = SceneWorkState::ready;
    for (auto node : nodes)
    {
        auto component = node.get_component<TerrainComponent>();
        if (!component || !component.enabled())
        {
            continue;
        }
        bool success = update(component, scope, budget);
        if (component.runtime()->streaming_ &&
            terminal(component.runtime()->streaming_->snapshot.state) &&
            component.runtime()->streaming_->snapshot.state != TerrainOperationState::completed)
        {
            success = false;
            component.runtime()->error_ = component.runtime()->streaming_->snapshot.error;
        }
        auto* state = component.runtime();
        SceneWorkProgress progress;
        progress.domain = "terrain";
        progress.stage = "initial_regions";
        progress.state = !success        ? SceneWorkState::failed
                         : state->ready_ ? SceneWorkState::ready
                                         : SceneWorkState::working;
        progress.error = state->error_;
        progress.error_code = success ? "" : "terrain_preparation_failed";
        if (!success)
        {
            status.state = SceneWorkState::failed;
        }
        else if (!state->ready_ && status.state != SceneWorkState::failed)
        {
            status.state = SceneWorkState::working;
        }
        status.stages.push_back(std::move(progress));
    }
    return status.state;
}
bool TerrainSystem::scope_status(SceneScopeId scope, ScenePreparationSnapshot& output) const
{
    output = {};
    const auto found = scopes_.find(scope);
    if (found == scopes_.end())
    {
        return false;
    }
    output = found->second;
    return true;
}
void TerrainSystem::release_scope(SceneScopeId scope) noexcept
{
    scopes_.erase(scope);
}
bool TerrainSystem::on_tick(double seconds)
{
    if (!world_.paused())
    {
        simulation_time_ += std::max(seconds, 0.0);
    }
    return synchronize();
}
bool TerrainSystem::synchronize()
{
    ENGINE_PROFILE_ZONE("Terrain.synchronize");
    if (!is_initialized() || !world_.is_initialized())
    {
        return false;
    }
    std::vector<std::uint64_t> owners, obsolete;
    flecs::world ecs(world_.native_world());
    ecs.each<const ecs_detail::ComponentStorage<TerrainComponent::Data>>(
        [&](flecs::entity entity, const auto&)
        {
            if (entity.has<ecs_detail::Published>())
            {
                owners.push_back(entity.id());
            }
        });
    ecs.each<const TerrainGenerated>(
        [&](flecs::entity entity, const TerrainGenerated& generated)
        {
            if (!entity.has<ecs_detail::Published>())
            {
                return;
            }
            const auto owner = world_.find_node(generated.owner).get_component<TerrainComponent>();
            if (!owner || owner.instance_id() != generated.instance)
            {
                obsolete.push_back(entity.id());
            }
        });
    for (auto id : obsolete)
    {
        auto node = world_.find_node(id);
        if (node)
        {
            (void)node.scene()->destroy_node(id);
        }
    }
    for (auto id : owners)
    {
        const auto component = world_.find_node(id).get_component<TerrainComponent>();
        if (component)
        {
            (void)update(component, {}, SceneWorkBudget{});
        }
    }
    // Per-owner failures are observable and never stop the render loop.
    return true;
}
void TerrainSystem::on_shutdown() noexcept
{
    if (!world_.native_world())
    {
        return;
    }
    std::vector<std::uint64_t> owners, generated;
    flecs::world ecs(world_.native_world());
    ecs.each<const ecs_detail::ComponentStorage<TerrainComponent::Data>>(
        [&](flecs::entity entity, const auto&) { owners.push_back(entity.id()); });
    ecs.each<const TerrainGenerated>([&](flecs::entity entity, const auto&)
                                     { generated.push_back(entity.id()); });
    for (auto id : owners)
    {
        auto component = world_.find_node(id).get_component<TerrainComponent>();
        if (!component)
        {
            continue;
        }
        auto* state = component.runtime();
        state->system_ = nullptr;
        state->streaming_.reset();
        state->operations_.clear();
        state->source_loading_.reset();
        state->material_loading_.reset();
        state->source_.reset();
        state->material_.reset();
        state->pending_source_.reset();
        state->pending_material_.reset();
        state->ready_ = false;
    }
    for (auto id : generated)
    {
        auto node = world_.find_node(id);
        if (node)
        {
            (void)node.scene()->destroy_node(id);
        }
    }
    scopes_.clear();
}
} // namespace engine::plugins::terrain
