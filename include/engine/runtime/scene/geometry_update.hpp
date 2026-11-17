#pragma once

#include "runtime/physics/mesh_collision_data.hpp"
#include "runtime/scene/collider.hpp"
#include "runtime/scene/mesh_renderer.hpp"
#include "runtime/scene/scene_loading.hpp"

#include <memory>
#include <string>
#include <vector>

namespace engine
{
class WorldSystem;
class RenderSystem;
class PhysicsSystem;
class TaskSystem;

struct RenderGeometryChange
{
    MeshRenderer target;
    std::uint64_t instance_id = 0;
    bool remove = false;
    std::shared_ptr<const schema::MeshDataT> mesh;
};

struct CollisionGeometryChange
{
    Collider target;
    std::uint64_t instance_id = 0;
    bool remove = false;
    schema::ColliderDataT settings;
    MeshCollisionData mesh;
};

struct GeometryUpdateSpec
{
    std::uint64_t world_epoch = 0;
    std::uint64_t scene_id = 0;
    SceneScopeId new_nodes_scope;
    std::vector<Node> retired_nodes;
    std::vector<RenderGeometryChange> render_changes;
    std::vector<CollisionGeometryChange> collision_changes;
};

struct GeometryUpdateSnapshot
{
    SceneWorkState state = SceneWorkState::queued;
    std::vector<SceneWorkProgress> stages;
    std::uint64_t staging_bytes = 0;
    std::string error_code;
};

struct GeometryPublication
{
    std::uint64_t scene_id = 0;
    std::uint64_t content_revision = 0;
    Bounds3 dirty_bounds;
};

class GeometryUpdate final
{
public:
    GeometryUpdate(WorldSystem& world, RenderSystem& renderer, PhysicsSystem& physics,
                   TaskSystem& tasks) noexcept;
    ~GeometryUpdate() noexcept;
    GeometryUpdate(const GeometryUpdate&) = delete;
    GeometryUpdate& operator=(const GeometryUpdate&) = delete;

    [[nodiscard]] bool begin(const GeometryUpdateSpec& spec);
    [[nodiscard]] SceneWorkState prepare(const SceneWorkBudget& budget);
    [[nodiscard]] bool status(GeometryUpdateSnapshot& output) const;
    [[nodiscard]] bool publish(GeometryPublication& output);
    [[nodiscard]] bool cancel();
    void release() noexcept;

private:
    struct State;
    std::unique_ptr<State> state_;
    WorldSystem& world_;
    RenderSystem& renderer_;
    PhysicsSystem& physics_;
    TaskSystem& tasks_;
};
} // namespace engine
