#include "plugins/runtime/terrain/terrain_fragment_runtime.hpp"
#include "plugins/runtime/terrain/terrain_islands.hpp"
#include "plugins/runtime/terrain/terrain_samples.hpp"
#include "runtime/scene/ecs_storage.hpp"
#include "runtime/scene/rigid_body.hpp"
#include "runtime/scene/scene.hpp"
#include "runtime/scene/transform.hpp"
#include "runtime/utils/hash_utils.hpp"
#include <algorithm>
#include <cmath>
#include <flatbuffers/flatbuffers.h>

namespace engine::plugins::terrain
{
using namespace terrain_detail;

bool restore_fragments(const TerrainReadSnapshot& source,
                       const schema::TerrainCheckpointDataT& checkpoint,
                       const TerrainStateStore& store, std::string_view slot,
                       const TerrainMesher& mesher, std::vector<TerrainFragment>& output,
                       std::string& error)
{
    output.clear();
    for (const auto& saved : checkpoint.debris)
    {
        auto index = std::make_shared<schema::TerrainAssetDataT>();
        index->origin = source.source->origin;
        index->voxel_size = source.source->voxel_size;
        index->block_cells = source.source->block_cells;
        index->bounds = saved->bounds;
        index->material_layers = source.source->material_layers;
        index->surface_material = source.source->surface_material;
        TerrainReadSnapshot samples;
        samples.source = index;
        samples.source_revision = checkpoint.revision;
        if (!enumerate(*index, index->bounds, 0, 256, samples.regions))
        {
            error = "Invalid saved debris bounds";
            return false;
        }
        for (auto key : samples.regions)
        {
            auto region = std::make_unique<schema::TerrainRegionEntryT>();
            region->key = key;
            region->uniform = schema::TerrainUniformState::air;
            index->regions.push_back(std::move(region));
        }
        for (const auto& block_ref : saved->blocks)
        {
            auto block = std::make_shared<schema::TerrainBlockDataT>();
            if (!store.read_block(slot, block_ref->payload_key, *block, error) ||
                block->key != block_ref->key)
            {
                return false;
            }
            samples.blocks[block->key] = std::move(block);
        }
        TerrainFragment piece;
        if (!rebuild_fragment(samples, mesher, piece, error))
        {
            return false;
        }
        piece.from_checkpoint = true;
        piece.stable_id = saved->stable_id;
        piece.source_region = saved->source_region;
        piece.source_regions = saved->source_regions;
        piece.sleeping = saved->sleeping;
        piece.position = saved->position;
        piece.linear_velocity = saved->linear_velocity;
        piece.angular_velocity = saved->angular_velocity;
        piece.density = saved->density;
        piece.age = saved->simulation_age;
        // Transform uses Euler degrees; this conversion preserves the normalized checkpoint
        // quaternion.
        const auto q = saved->rotation;
        const double degrees = 180.0 / 3.14159265358979323846;
        piece.rotation = {std::atan2(2 * (q.w() * q.x() + q.y() * q.z()),
                                     1 - 2 * (q.x() * q.x() + q.y() * q.y())) *
                              degrees,
                          std::asin(std::clamp(2 * (q.w() * q.y() - q.z() * q.x()), -1.0, 1.0)) *
                              degrees,
                          std::atan2(2 * (q.w() * q.z() + q.x() * q.y()),
                                     1 - 2 * (q.y() * q.y() + q.z() * q.z())) *
                              degrees};
        output.push_back(std::move(piece));
    }
    return true;
}

bool create_fragments(Scene& scene, TerrainComponent owner, const TransformSettings& pose,
                      const schema::TerrainDataT& settings,
                      std::span<const TerrainFragment> fragments, std::vector<Node>& output,
                      std::string& error)
{
    output.clear();
    for (const auto& fragment : fragments)
    {
        auto node = scene.create_node("Terrain fragment");
        TransformSettings transform;
        transform.position = fragment.from_checkpoint
                                 ? fragment.position
                                 : Vec3{pose.position.x() + fragment.position.x() * pose.scale.x(),
                                        pose.position.y() + fragment.position.y() * pose.scale.x(),
                                        pose.position.z() + fragment.position.z() * pose.scale.x()};
        transform.scale = pose.scale;
        transform.rotation = fragment.rotation;
        if (!node || !node.set_transient(true) ||
            !node.get_component<Transform>().set_settings(transform))
        {
            error = "Cannot create terrain fragment transform";
            return false;
        }
        auto renderer = node.add_component<MeshRenderer>();
        auto collider = node.add_component<Collider>();
        auto body = node.add_component<RigidBody>();
        schema::RigidBodyDataT motion;
        motion.motion = schema::BodyMotion::dynamic;
        motion.mass = float(std::clamp(
            fragment.volume * fragment.density * std::pow(pose.scale.x(), 3), 0.01, 1e9));
        motion.linear_velocity = fragment.linear_velocity;
        motion.angular_velocity = fragment.angular_velocity;
        if (!renderer || !collider || !body || !renderer.set_transient(true) ||
            !renderer.set_mesh_data(fragment.mesh) ||
            !renderer.material().set_type(settings.surface_material_guid.empty()
                                              ? "builtin://materials/default-pbr"
                                              : settings.surface_material_guid) ||
            !collider.set_transient(true) || !collider.set_settings(fragment.collision) ||
            !body.set_transient(true) || !body.set_settings(motion) ||
            (fragment.from_checkpoint && !body.set_sleeping(fragment.sleeping)))
        {
            error = "Cannot prepare terrain fragment render or collision data";
            return false;
        }
        if (fragment.samples.source && !fragment.samples.source->surface_material.empty() &&
            (!renderer.material().set_type(fragment.samples.source->surface_material) ||
             !renderer.material().set_v3("surfaceOrigin", fragment.bounds.minimum())))
        {
            error = "Cannot bind fragment surface material";
            return false;
        }
        flecs::world(scene.world().native_world())
            .entity(node.id())
            .set<TerrainGenerated>({owner.node().id(), owner.instance_id()});
        output.push_back(node);
    }
    return true;
}

bool capture_fragments(std::span<const TerrainComponentRuntime::Fragment> fragments, double time,
                       schema::TerrainCheckpointDataT& output,
                       std::vector<schema::TerrainBlockDataT>& blocks, std::string& error)
{
    output.debris.clear();
    for (const auto& fragment : fragments)
    {
        TransformSettings transform;
        schema::RigidBodyDataT motion;
        const auto body = fragment.node.get_component<RigidBody>();
        if (!fragment.node || !fragment.node.get_component<Transform>().get_settings(transform) ||
            !body.get_settings(motion))
        {
            error = "Cannot capture a stale terrain fragment";
            return false;
        }
        auto saved = std::make_unique<schema::TerrainDebrisStateT>();
        if (!body.get_sleeping(saved->sleeping))
        {
            error = "Terrain fragment physics state is not synchronized";
            return false;
        }
        saved->stable_id = fragment.stable_id;
        saved->source_region = fragment.source_region;
        saved->source_regions = fragment.source_regions;
        saved->bounds = fragment.bounds;
        saved->position = transform.position;
        const double radians = 3.14159265358979323846 / 360;
        const double cx = std::cos(transform.rotation.x() * radians),
                     sx = std::sin(transform.rotation.x() * radians);
        const double cy = std::cos(transform.rotation.y() * radians),
                     sy = std::sin(transform.rotation.y() * radians);
        const double cz = std::cos(transform.rotation.z() * radians),
                     sz = std::sin(transform.rotation.z() * radians);
        saved->rotation = {sx * cy * cz - cx * sy * sz, cx * sy * cz + sx * cy * sz,
                           cx * cy * sz - sx * sy * cz, cx * cy * cz + sx * sy * sz};
        saved->linear_velocity = body.linear_velocity();
        saved->angular_velocity = motion.angular_velocity;
        saved->density = fragment.density;
        saved->simulation_age = std::max(0.0, time - fragment.published_at);
        for (const auto& [key, block] : fragment.samples.blocks)
        {
            flatbuffers::FlatBufferBuilder builder;
            schema::FinishTerrainBlockDataBuffer(
                builder, schema::TerrainBlockData::Pack(builder, block.get()));
            const auto bytes =
                std::as_bytes(std::span(builder.GetBufferPointer(), builder.GetSize()));
            auto value = std::make_unique<schema::TerrainRegionStateT>();
            value->key = key;
            value->payload_key = value->content_hash = hash::hex(hash::fnv1a(bytes));
            saved->blocks.push_back(std::move(value));
            blocks.push_back(*block);
        }
        output.debris.push_back(std::move(saved));
    }
    return true;
}
} // namespace engine::plugins::terrain
