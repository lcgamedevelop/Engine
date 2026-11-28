#include "runtime/scene/geometry_update.hpp"
#include "runtime/resource/mesh_storage.hpp"
#include "runtime/performance/function_profile.hpp"
#include "runtime/performance/profiling.hpp"

#include "runtime/physics/physics_system.hpp"
#include "runtime/render/render_system.hpp"
#include "runtime/scene/collider_runtime.hpp"
#include "runtime/scene/ecs_storage.hpp"
#include "runtime/scene/material_state.hpp"
#include "runtime/scene/mesh_renderer_runtime.hpp"
#include "runtime/scene/rigid_body.hpp"
#include "runtime/scene/scene.hpp"
#include "runtime/scene/transform.hpp"
#include "runtime/task/task_system.hpp"
#include "runtime/world/world_scene_scope.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <utility>

namespace engine
{
namespace
{
constexpr std::size_t maximum_changes = 1024;
constexpr std::size_t maximum_nodes = 1024;
constexpr std::uint64_t maximum_staging_bytes = 128ULL * 1024 * 1024;

bool complete(SceneWorkState state)
{
    ENGINE_PROFILE_FUNCTION();
    return state == SceneWorkState::ready || state == SceneWorkState::not_required;
}
} // namespace

struct GeometryUpdate::State
{
    struct Pose
    {
        Node node;
        Node parent;
        Transform transform;
        TransformSettings settings;
        std::array<double, 16> matrix{};
        bool enabled = false;
        bool visible = false;
    };
    struct Render
    {
        RenderGeometryChange change;
        MeshRenderer prepared;
        schema::ComponentDataT original;
        std::vector<schema::MaterialDataT> materials;
        std::uint64_t revision = 0;
    };
    struct Collision
    {
        CollisionGeometryChange change;
        Collider prepared;
        schema::ComponentDataT original;
        std::shared_ptr<const MeshCollisionData> original_parts;
        std::shared_ptr<const schema::MeshDataT> original_mesh;
        RigidBody rigid;
        schema::ComponentDataT rigid_data;
    };

    GeometryUpdateSpec spec;
    GeometryUpdateSnapshot snapshot;
    SceneScopeId staging;
    std::vector<Render> renders;
    std::vector<Collision> collisions;
    std::vector<Pose> poses;
    std::vector<Node> retired;
    std::vector<Collider> new_colliders;
    std::map<std::uint64_t, Node> mirrors;
    std::size_t next_render = 0;
    std::size_t next_collision = 0;
    std::size_t phase = 0;
    std::uint32_t submitted_frame = 0;
    bool published = false;
    std::uint64_t device_generation = 0;
    Bounds3 bounds;

    SceneWorkState fail(std::string code)
    {
        ENGINE_PROFILE_FUNCTION();
        snapshot.state = SceneWorkState::failed;
        snapshot.error_code = std::move(code);
        return snapshot.state;
    }
};

GeometryUpdate::GeometryUpdate(WorldSystem& world, RenderSystem& renderer, PhysicsSystem& physics,
                               TaskSystem& tasks) noexcept
    : world_(world), renderer_(renderer), physics_(physics), tasks_(tasks)
{
    ENGINE_PROFILE_FUNCTION();
}

GeometryUpdate::~GeometryUpdate() noexcept
{
    ENGINE_PROFILE_FUNCTION();
    release();
}

bool GeometryUpdate::begin(const GeometryUpdateSpec& spec)
{
    ENGINE_PROFILE_FUNCTION();
    if (state_ || !world_.can_modify() || !world_.is_initialized() || !renderer_.is_initialized() ||
        !physics_.is_initialized() || !tasks_.is_initialized() ||
        spec.world_epoch != world_.epoch() || !world_.scene() ||
        spec.scene_id != world_.scene()->id() ||
        spec.render_changes.size() + spec.collision_changes.size() > maximum_changes ||
        spec.retired_nodes.size() > maximum_changes ||
        (spec.render_changes.empty() && spec.collision_changes.empty() &&
         spec.retired_nodes.empty() && spec.new_nodes_scope == SceneScopeId{}))
    {
        return false;
    }
    Scene* additions = nullptr;
    if (spec.new_nodes_scope != SceneScopeId{})
    {
        const auto found = world_.scopes_.find(spec.new_nodes_scope.value);
        if (spec.new_nodes_scope.world_epoch != world_.epoch() || found == world_.scopes_.end() ||
            found->second->published || found->second->sealed || !found->second->restored ||
            found->second->kind != SceneLoadKind::append_batch ||
            found->second->destination != spec.scene_id)
        {
            return false;
        }
        additions = found->second->candidate.get();
    }
    auto candidate = std::make_unique<State>();
    candidate->spec = spec;
    candidate->device_generation = renderer_.device_generation_;
    candidate->snapshot.state = SceneWorkState::working;
    const auto owned = [&](Node node)
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        return node && node.world() == &world_ &&
               (node.scene() == world_.scene() || node.scene() == additions);
    };
    std::set<std::uint64_t> retired;
    std::vector<Node> pending = spec.retired_nodes;
    for (std::size_t next = 0; next < pending.size(); ++next)
    {
        const auto node = pending[next];
        if (!owned(node) || node.scene() == additions || pending.size() > maximum_nodes)
        {
            return false;
        }
        if (!retired.insert(node.id()).second)
        {
            continue;
        }
        candidate->retired.push_back(node);
        flecs::world(world_.native_world())
            .entity(node.id())
            .children([&](flecs::entity child)
                      {
                ENGINE_PROFILE_FUNCTION_PROBE();
                pending.push_back(node.scene()->find_node(child.id()));
            });
    }
    auto render_changes = spec.render_changes;
    auto collision_changes = spec.collision_changes;
    for (auto node : candidate->retired)
    {
        if (auto renderer = node.get_component<MeshRenderer>())
        {
            render_changes.push_back({renderer, renderer.instance_id(), true, {}});
        }
        if (auto collider = node.get_component<Collider>())
        {
            collision_changes.push_back({collider, collider.instance_id(), true, {}, {}});
        }
    }
    if (render_changes.size() + collision_changes.size() > maximum_changes)
    {
        return false;
    }
    std::set<std::pair<std::uint64_t, ComponentType>> identities;
    std::set<std::uint64_t> poses;
    const auto capture_pose = [&](Node node)
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        for (auto current = node; current; current = current.parent())
        {
            if (poses.contains(current.id()))
            {
                break;
            }
            if (poses.size() >= maximum_nodes)
            {
                return false;
            }
            const auto* drive = flecs::world(world_.native_world())
                                    .entity(current.id())
                                    .try_get<ecs_detail::TransformDrive>();
            State::Pose pose;
            pose.node = current;
            pose.parent = current.parent();
            pose.transform = current.get_component<Transform>();
            pose.enabled = current.enabled();
            pose.visible = current.visible();
            if ((drive && drive->active) || !pose.transform.get_settings(pose.settings) ||
                !pose.transform.world_matrix(pose.matrix.data()))
            {
                return false;
            }
            poses.insert(current.id());
            candidate->poses.push_back(std::move(pose));
        }
        return true;
    };
    std::array<double, 3> minimum{INFINITY, INFINITY, INFINITY};
    std::array<double, 3> maximum{-INFINITY, -INFINITY, -INFINITY};
    const auto include_point = [&](Node node, double x, double y, double z)
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        const auto pose = std::find_if(candidate->poses.begin(), candidate->poses.end(),
                                       [&](const auto& item)
                                       {
                                           ENGINE_PROFILE_FUNCTION_PROBE();
                                           return item.node == node;
                                       });
        const auto& matrix = pose->matrix;
        for (std::size_t axis = 0; axis < 3; ++axis)
        {
            const double value =
                matrix[axis] * x + matrix[4 + axis] * y + matrix[8 + axis] * z + matrix[12 + axis];
            minimum[axis] = std::min(minimum[axis], value);
            maximum[axis] = std::max(maximum[axis], value);
        }
    };
    const auto include_mesh = [&](Node node, const schema::MeshDataT* mesh)
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        if (mesh)
        {
            for (const auto& vertex : mesh->vertices)
            {
                include_point(node, vertex.x(), vertex.y(), vertex.z());
            }
        }
    };
    const auto include_collider = [&](Node node, const schema::ColliderDataT& data)
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        const double radius = std::max(data.radius, 0.0f);
        const std::array<double, 3> extent{
            std::max(data.half_extents.x(), radius),
            std::max(data.half_extents.y(), radius + data.half_height),
            std::max(data.half_extents.z(), radius)};
        for (int corner = 0; corner < 8; ++corner)
        {
            include_point(node, data.center.x() + (corner & 1 ? 1 : -1) * extent[0],
                          data.center.y() + (corner & 2 ? 1 : -1) * extent[1],
                          data.center.z() + (corner & 4 ? 1 : -1) * extent[2]);
        }
        for (const auto& hull : data.hulls)
        {
            if (!hull)
            {
                continue;
            }
            for (const auto& point : hull->vertices)
            {
                include_point(node, point.x() + data.center.x(), point.y() + data.center.y(),
                              point.z() + data.center.z());
            }
        }
    };
    for (const auto& change : render_changes)
    {
        if (!change.target || change.target.instance_id() != change.instance_id ||
            !owned(change.target.node()) ||
            !identities.emplace(change.target.node().id(), change.target.type()).second ||
            !capture_pose(change.target.node()) ||
            change.target.mesh_revision() == std::numeric_limits<std::uint64_t>::max() ||
            (!change.remove &&
             (!change.mesh || !change.mesh->skin.empty() || !change.mesh->skeleton_guid.empty())))
        {
            return false;
        }
        State::Render record;
        record.change = change;
        record.revision = change.target.mesh_revision();
        if (!change.target.get_data(record.original))
        {
            return false;
        }
        include_mesh(change.target.node(), change.target.mesh_data());
        if (!change.remove)
        {
            include_mesh(change.target.node(), change.mesh.get());
        }
        const auto* runtime = ecs_detail::component_runtime(change.target);
        for (const auto& material : runtime->materials_)
        {
            record.materials.push_back(material.data_);
        }
        if (change.mesh)
        {
            candidate->snapshot.staging_bytes += change.mesh->vertices.size() * 96ULL +
                                                 mesh_storage::indices(*change.mesh).size() * 4ULL;
        }
        candidate->renders.push_back(std::move(record));
    }
    for (const auto& change : collision_changes)
    {
        if (!change.target || change.target.instance_id() != change.instance_id ||
            !owned(change.target.node()) ||
            !identities.emplace(change.target.node().id(), change.target.type()).second ||
            !capture_pose(change.target.node()) ||
            (!change.remove && !Collider::valid_settings(change.settings, nullptr, &change.mesh)))
        {
            return false;
        }
        State::Collision record;
        record.change = change;
        record.rigid = change.target.node().get_component<RigidBody>();
        if (!change.target.get_data(record.original) ||
            (record.rigid && !record.rigid.get_data(record.rigid_data)))
        {
            return false;
        }
        const auto* runtime = ecs_detail::component_runtime(change.target);
        record.original_parts = runtime->mesh_parts_;
        record.original_mesh = runtime->transient_mesh_;
        include_collider(change.target.node(), *record.original.payload.AsColliderData());
        include_mesh(change.target.node(), change.target.mesh_data());
        if (record.original_parts)
        {
            for (const auto& part : record.original_parts->parts)
            {
                include_mesh(change.target.node(), part.get());
            }
        }
        if (!change.remove)
        {
            include_collider(change.target.node(), change.settings);
        }
        for (const auto& part : change.mesh.parts)
        {
            if (part)
            {
                if (!change.remove)
                {
                    include_mesh(change.target.node(), part.get());
                }
                candidate->snapshot.staging_bytes +=
                    part->vertices.size() * 24ULL + mesh_storage::indices(*part).size() * 4ULL;
            }
        }
        candidate->collisions.push_back(std::move(record));
    }
    if (additions)
    {
        std::vector<Node> nodes;
        if (!additions->collect_nodes(nodes) || nodes.size() > maximum_nodes)
        {
            return false;
        }
        for (auto node : nodes)
        {
            if (!capture_pose(node))
            {
                return false;
            }
            if (auto mesh = node.get_component<MeshRenderer>())
            {
                include_mesh(node, mesh.mesh_data());
            }
            if (auto collider = node.get_component<Collider>())
            {
                schema::ColliderDataT data;
                if (!collider.get_settings(data))
                {
                    return false;
                }
                include_collider(node, data);
                include_mesh(node, collider.mesh_data());
            }
        }
    }
    if (std::isfinite(minimum[0]))
    {
        candidate->bounds =
            Bounds3({minimum[0], minimum[1], minimum[2]}, {maximum[0], maximum[1], maximum[2]});
    }
    if (candidate->snapshot.staging_bytes > maximum_staging_bytes ||
        !world_.begin_scope(SceneLoadKind::append_batch, spec.scene_id, candidate->staging))
    {
        return false;
    }
    if (additions && !world_.freeze_scope(spec.new_nodes_scope))
    {
        world_.release_scope(candidate->staging);
        return false;
    }
    state_ = std::move(candidate);
    return true;
}

SceneWorkState GeometryUpdate::prepare(const SceneWorkBudget& budget)
{
    ENGINE_PROFILE_FUNCTION();
    if (!state_)
    {
        return SceneWorkState::failed;
    }
    auto& value = *state_;
    if (value.snapshot.state != SceneWorkState::working)
    {
        return value.snapshot.state;
    }
    if (!world_.scene() || world_.epoch() != value.spec.world_epoch ||
        world_.scene()->id() != value.spec.scene_id || !world_.can_modify() ||
        !std::isfinite(budget.seconds) || budget.seconds <= 0 || budget.items == 0 ||
        budget.upload_bytes == 0)
    {
        return value.fail("invalid_geometry_context");
    }
    if (renderer_.device_generation_ != value.device_generation)
    {
        return value.fail("geometry_device_changed");
    }
    if (!renderer_.gpu_ready())
    {
        return SceneWorkState::working;
    }
    const auto started = std::chrono::steady_clock::now();
    auto step = budget;
    const auto remaining = [&]()
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        step.seconds =
            budget.seconds -
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        return step.seconds > 0;
    };
    const auto mirror = [&](Node source, auto&& self, Scene& candidate) -> Node
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        if (const auto found = value.mirrors.find(source.id()); found != value.mirrors.end())
        {
            return found->second;
        }
        const auto pose = std::find_if(value.poses.begin(), value.poses.end(),
                                       [&](const auto& item)
                                       {
                                           ENGINE_PROFILE_FUNCTION_PROBE();
                                           return item.node == source;
                                       });
        if (pose == value.poses.end())
        {
            return {};
        }
        Node parent;
        if (pose->parent)
        {
            parent = self(pose->parent, self, candidate);
            if (!parent)
            {
                return {};
            }
        }
        auto node = candidate.create_node("Geometry candidate");
        if (!node || !node.get_component<Transform>().set_settings(pose->settings) ||
            (parent && !node.set_parent(parent)) || !node.set_enabled(pose->enabled) ||
            !node.set_visible(pose->visible) || !node.set_transient(true))
        {
            return {};
        }
        value.mirrors.emplace(source.id(), node);
        return node;
    };
    const auto prepare_render = [&](State::Render& record, Scene& candidate)
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        auto source = record.change.target;
        auto node = mirror(source.node(), mirror, candidate);
        auto prepared = node ? node.add_component<MeshRenderer>() : MeshRenderer{};
        if (!source || !prepared)
        {
            return false;
        }
        auto* from = ecs_detail::component_runtime(source);
        auto* to = ecs_detail::component_runtime(prepared);
        if (!prepared.set_material_count(from->materials_.size()))
        {
            return false;
        }
        for (std::size_t slot = 0; slot < from->materials_.size(); ++slot)
        {
            MaterialState material(node);
            const auto& original = from->materials_[slot];
            if (!material.material_resource_.retain(original.material_resource_) ||
                !material.shader_resource_.retain(original.shader_resource_))
            {
                return false;
            }
            material.data_ = original.data_;
            material.identity = to->materials_[slot].identity;
            for (const auto& [name, texture] : original.textures_)
            {
                if (!material.textures_[name].retain(texture))
                {
                    return false;
                }
            }
            to->materials_[slot] = std::move(material);
        }
        if (!record.change.remove && !prepared.set_mesh_data(*record.change.mesh))
        {
            return false;
        }
        if (!prepared.set_enabled(source.enabled()) ||
            !prepared.set_visible(source.visible() && !record.change.remove))
        {
            return false;
        }
        if (record.change.remove)
        {
            to->mesh_resource_.reset();
        }
        record.prepared = prepared;
        return true;
    };
    const auto prepare_collision = [&](State::Collision& record, Scene& candidate)
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        const auto source = record.change.target;
        auto node = mirror(source.node(), mirror, candidate);
        auto prepared = node ? node.add_component<Collider>() : Collider{};
        if (!source || !prepared)
        {
            return false;
        }
        if (!record.change.remove)
        {
            *prepared.write_data<Collider::Data>() = record.change.settings;
            auto* runtime = ecs_detail::component_runtime(prepared);
            if (record.change.settings.shape == schema::ColliderShape::mesh)
            {
                runtime->mesh_parts_ =
                    std::make_shared<const MeshCollisionData>(record.change.mesh);
            }
            if (record.rigid)
            {
                auto rigid = node.add_component<RigidBody>();
                if (!rigid || !rigid.restore_data(record.rigid_data))
                {
                    return false;
                }
            }
        }
        if (record.change.remove)
        {
            *prepared.write_data<Collider::Data>() = *record.original.payload.AsColliderData();
            ecs_detail::component_runtime(prepared)->geometry_removed_ = true;
        }
        if (!prepared.set_enabled(source.enabled() && !record.change.remove))
        {
            return false;
        }
        record.prepared = prepared;
        return true;
    };
    if (value.phase == 0)
    {
        std::size_t count = 0;
        while (count++ < budget.items && remaining())
        {
            bool accepted = true;
            if (value.next_render < value.renders.size())
            {
                accepted = world_.with_scope_write(
                    value.staging, [&](Scene& scene)
                    {
                        ENGINE_PROFILE_FUNCTION_PROBE();
                        return prepare_render(value.renders[value.next_render], scene);
                    });
                ++value.next_render;
            }
            else if (value.next_collision < value.collisions.size())
            {
                accepted = world_.with_scope_write(
                    value.staging, [&](Scene& scene)
                    {
                        ENGINE_PROFILE_FUNCTION_PROBE();
                        return prepare_collision(value.collisions[value.next_collision], scene);
                    });
                ++value.next_collision;
            }
            else
            {
                if (!world_.seal_scope(value.staging) ||
                    (value.spec.new_nodes_scope != SceneScopeId{} &&
                     !world_.seal_scope(value.spec.new_nodes_scope)))
                {
                    return value.fail("geometry_scope_seal_failed");
                }
                value.phase = 1;
                break;
            }
            if (!accepted)
            {
                return value.fail("geometry_candidate_failed");
            }
        }
    }
    value.snapshot.stages.clear();
    SceneWorkProgress preparation;
    preparation.domain = "geometry";
    preparation.stage = "candidates";
    preparation.total = value.renders.size() + value.collisions.size();
    preparation.completed = value.next_render + value.next_collision;
    preparation.total_known = true;
    preparation.state = value.phase > 0 ? SceneWorkState::ready : SceneWorkState::working;
    value.snapshot.stages.push_back(preparation);
    if (value.phase == 1)
    {
        bool ready = true;
        for (const auto scope : {value.staging, value.spec.new_nodes_scope})
        {
            if (scope == SceneScopeId{})
            {
                continue;
            }
            ScenePreparationSnapshot status;
            const auto run = [&](auto& system, auto invoke)
            {
                ENGINE_PROFILE_FUNCTION_PROBE();
                if (!ready || !remaining())
                {
                    ready = false;
                    return;
                }
                const auto state = invoke();
                if (!system.scope_status(scope, status))
                {
                    value.fail("geometry_preparation_status_failed");
                    ready = false;
                    return;
                }
                value.snapshot.stages.insert(value.snapshot.stages.end(), status.stages.begin(),
                                             status.stages.end());
                if (state == SceneWorkState::failed || state == SceneWorkState::cancelled)
                {
                    value.fail("geometry_preparation_failed");
                }
                ready = ready && complete(state);
            };
            run(world_, [&]()
            {
                ENGINE_PROFILE_FUNCTION_PROBE();
                return world_.step_scope(scope, step);
            });
            run(physics_, [&]()
            {
                ENGINE_PROFILE_FUNCTION_PROBE();
                return physics_.prepare_scope(scope, step);
            });
            run(renderer_, [&]()
            {
                ENGINE_PROFILE_FUNCTION_PROBE();
                return renderer_.prepare_scope(scope, step);
            });
        }
        if (ready)
        {
            if (value.spec.new_nodes_scope != SceneScopeId{})
            {
                auto* candidate = world_.scope_candidate(value.spec.new_nodes_scope);
                std::vector<Node> nodes;
                if (!candidate || !candidate->collect_nodes(nodes) || nodes.size() > maximum_nodes)
                {
                    return value.fail("geometry_new_nodes_invalid");
                }
                value.new_colliders.clear();
                for (auto node : nodes)
                {
                    if (auto collider = node.get_component<Collider>())
                    {
                        value.new_colliders.push_back(collider);
                    }
                }
            }
            world_.scenes_.reserve(world_.scenes_.size() + 1);
            world_.retirement_.reserve(world_.retirement_.size() + value.spec.retired_nodes.size());
            world_.geometry_notifications_.reserve(world_.geometry_notifications_.size() +
                                                   value.renders.size() + value.collisions.size());
            value.submitted_frame = renderer_.frame_number();
            value.phase = 2;
        }
    }
    if (value.phase == 2 && renderer_.gpu_ready() &&
        renderer_.frame_number() != value.submitted_frame)
    {
        value.snapshot.state = SceneWorkState::ready;
    }
    return value.snapshot.state;
}

bool GeometryUpdate::status(GeometryUpdateSnapshot& output) const
{
    ENGINE_PROFILE_FUNCTION();
    output = state_ ? state_->snapshot : GeometryUpdateSnapshot{};
    return state_ != nullptr;
}

bool GeometryUpdate::publish(GeometryPublication& output)
{
    ENGINE_PROFILE_FUNCTION();
    output = {};
    if (!state_ || state_->published || state_->snapshot.state != SceneWorkState::ready ||
        state_->device_generation != renderer_.device_generation_ || !renderer_.gpu_ready() ||
        !world_.can_modify() || world_.paused() || world_.epoch() != state_->spec.world_epoch ||
        !world_.scene() || world_.scene()->id() != state_->spec.scene_id)
    {
        return false;
    }
    auto& value = *state_;
    for (const auto& pose : value.poses)
    {
        TransformSettings settings;
        std::array<double, 16> matrix{};
        if (!pose.transform.get_settings(settings) || settings != pose.settings ||
            pose.node.parent() != pose.parent || pose.node.enabled() != pose.enabled ||
            pose.node.visible() != pose.visible || !pose.transform.world_matrix(matrix.data()) ||
            matrix != pose.matrix)
        {
            value.fail("geometry_transform_changed");
            return false;
        }
    }
    for (const auto& record : value.renders)
    {
        schema::ComponentDataT data;
        auto target = record.change.target;
        if (!target.get_data(data) || data != record.original ||
            target.mesh_revision() != record.revision ||
            !renderer_.can_publish_geometry(record.prepared, target))
        {
            value.fail("geometry_render_changed");
            return false;
        }
        const auto& materials = ecs_detail::component_runtime(target)->materials_;
        if (materials.size() != record.materials.size())
        {
            value.fail("geometry_material_changed");
            return false;
        }
        for (std::size_t slot = 0; slot < materials.size(); ++slot)
        {
            if (materials[slot].data_ != record.materials[slot])
            {
                value.fail("geometry_material_changed");
                return false;
            }
        }
    }
    for (const auto& record : value.collisions)
    {
        schema::ComponentDataT data;
        auto target = record.change.target;
        const auto* runtime = target ? ecs_detail::component_runtime(target) : nullptr;
        if (!runtime || !target.get_data(data) || data != record.original ||
            runtime->mesh_parts_ != record.original_parts ||
            runtime->transient_mesh_ != record.original_mesh ||
            !physics_.can_publish_geometry(record.prepared, target) ||
            target.node().get_component<RigidBody>() != record.rigid ||
            (record.rigid && (!record.rigid.get_data(data) || data != record.rigid_data)))
        {
            value.fail("geometry_collision_changed");
            return false;
        }
    }
    auto ecs = flecs::world(world_.native_world());
    auto& scope_state =
        ecs.entity(world_.scene()->metadata_entity_).get_mut<ecs_detail::ScopeState>();
    if (scope_state.content_revision == UINT64_MAX)
    {
        return false;
    }
    const auto revision = scope_state.content_revision + 1;
    if (value.spec.new_nodes_scope != SceneScopeId{})
    {
        auto* candidate = world_.scope_candidate(value.spec.new_nodes_scope);
        if (!candidate)
        {
            return false;
        }
        for (auto collider : value.new_colliders)
        {
            if (!physics_.can_publish_geometry(collider, collider))
            {
                return false;
            }
        }
        if (!world_.publish_scope(value.spec.new_nodes_scope) ||
            !world_.activate_scope(value.spec.new_nodes_scope))
        {
            return false;
        }
    }
    for (auto& record : value.renders)
    {
        auto target = record.change.target;
        auto* current = ecs_detail::component_runtime(target);
        auto* prepared = ecs_detail::component_runtime(record.prepared);
        std::swap(current->mesh_resource_, prepared->mesh_resource_);
        std::swap(current->lightmap_resource_, prepared->lightmap_resource_);
        current->geometry_removed_ = record.change.remove;
        current->skin_joint_count_ = 0;
        current->deformed_vertices_.swap(prepared->deformed_vertices_);
        current->deformed_tangents_.swap(prepared->deformed_tangents_);
        current->skinning_palette_.swap(prepared->skinning_palette_);
        current->skinning_normal_palette_.swap(prepared->skinning_normal_palette_);
        ++current->mesh_revision_;
        auto* data = target.write_data<MeshRenderer::Data>();
        data->mesh_guid.clear();
        data->lightmap_guid.clear();
        data->lightmap_mesh_hash.clear();
        data->lightmap_transform.clear();
        ++target.state()->revision;
        renderer_.publish_geometry(record.prepared, target);
        world_.geometry_notifications_.push_back(target);
    }
    for (auto& record : value.collisions)
    {
        auto target = record.change.target;
        auto* current = ecs_detail::component_runtime(target);
        auto* prepared = ecs_detail::component_runtime(record.prepared);
        std::swap(*current, *prepared);
        std::swap(*target.write_data<Collider::Data>(),
                  ecs.entity(record.prepared.node().id())
                      .get_mut<ecs_detail::ComponentStorage<Collider::Data>>()
                      .data);
        ++target.state()->revision;
        physics_.publish_geometry(record.prepared, target);
        world_.geometry_notifications_.push_back(target);
    }
    for (auto collider : value.new_colliders)
    {
        physics_.publish_geometry(collider, collider);
    }
    for (auto node : value.retired)
    {
        ecs.entity(node.id()).get_mut<schema::NodePropertiesT>().enabled = false;
    }
    for (auto node : value.spec.retired_nodes)
    {
        world_.retirement_.emplace_back(node, false);
    }
    ecs.entity(world_.scene()->metadata_entity_)
        .get_mut<ecs_detail::ScopeState>()
        .content_revision = revision;
    world_.invalidate_scene_state();
    value.published = true;
    output.scene_id = value.spec.scene_id;
    output.content_revision = revision;
    output.dirty_bounds = value.bounds;
    return true;
}

bool GeometryUpdate::cancel()
{
    ENGINE_PROFILE_FUNCTION();
    if (!state_ || state_->published)
    {
        return false;
    }
    state_->snapshot.state = SceneWorkState::cancelled;
    return true;
}

void GeometryUpdate::release() noexcept
{
    ENGINE_PROFILE_FUNCTION();
    if (!state_)
    {
        return;
    }
    physics_.release_scope(state_->staging);
    renderer_.release_scope(state_->staging);
    world_.release_scope(state_->staging);
    if (state_->spec.new_nodes_scope != SceneScopeId{})
    {
        physics_.release_scope(state_->spec.new_nodes_scope);
        renderer_.release_scope(state_->spec.new_nodes_scope);
        world_.release_scope(state_->spec.new_nodes_scope);
    }
    state_.reset();
}
} // namespace engine
