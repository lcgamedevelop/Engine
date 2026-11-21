#include "runtime/world/world_system.hpp"
#include "runtime/world/world_scene_scope.hpp"

#include "runtime/log/log_system.hpp"
#include "runtime/performance/memory_profiling.hpp"
#include "runtime/performance/function_profile.hpp"
#include "runtime/performance/profiling.hpp"
#include "runtime/resource/types/scene_resource.hpp"
#include "runtime/scene/ecs_storage.hpp"
#include "runtime/scene/resource_binding.hpp"
#include "runtime/scene/scene.hpp"
#include "runtime/scene/transform.hpp"

#include <flecs.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <mutex>
#include <new>
#include <numbers>
#include <string>
#include <utility>

namespace engine
{

#if defined(ENGINE_ENABLE_MEMORY_PROFILING)
namespace
{
void enable_flecs_memory_profiling()
{
    ENGINE_PROFILE_FUNCTION();
    static std::once_flag installed;
    std::call_once(installed,
                   []
                   {
                       const auto current = ecs_os_get_api();
                       // Flecs may already be in use by the host. Any existing heap can
                       // have live blocks from before this WorldSystem was initialized.
                       if (current.malloc_ || current.calloc_ || current.realloc_ ||
                           current.free_ || current.strdup_)
                       {
                           return;
                       }
                       using memory_profile::BackingAllocation;
                       constexpr auto pool = memory_profile::Pool::flecs;
                       ecs_os_set_api_defaults();
                       auto api = ecs_os_get_api();
                       api.malloc_ = [](ecs_size_t size) noexcept -> void*
                       {
                           if (size <= 0)
                           {
                               return nullptr;
                           }
                           ecs_os_linc(&ecs_os_api_malloc_count);
                           return BackingAllocation::allocate_block(
                               pool, static_cast<std::size_t>(size));
                       };
                       api.calloc_ = [](ecs_size_t size) noexcept -> void*
                       {
                           if (size <= 0)
                           {
                               return nullptr;
                           }
                           ecs_os_linc(&ecs_os_api_calloc_count);
                           return BackingAllocation::allocate_block(
                               pool, static_cast<std::size_t>(size), true);
                       };
                       api.free_ = [](void* address) noexcept
                       {
                           if (address)
                           {
                               ecs_os_linc(&ecs_os_api_free_count);
                               BackingAllocation::free_block(pool, address);
                           }
                       };
                       api.realloc_ = [](void* address, ecs_size_t size) noexcept -> void*
                       {
                           if (size < 0)
                           {
                               return nullptr;
                           }
                           if (size == 0)
                           {
                               ecs_os_free(address);
                               return nullptr;
                           }
                           if (address)
                           {
                               ecs_os_linc(&ecs_os_api_realloc_count);
                           }
                           else
                           {
                               ecs_os_linc(&ecs_os_api_malloc_count);
                           }
                           return BackingAllocation::reallocate_block(
                               pool, address, static_cast<std::size_t>(size), 0);
                       };
                       ecs_os_set_api(&api);
                   });
}
} // namespace
#endif

WorldSystem::WorldSystem(ResourceLoader* resources, const SceneRestoreConfig& config) noexcept
    : resources_(resources), restore_config_(config)
{
    ENGINE_PROFILE_FUNCTION();
}

WorldSystem::~WorldSystem() noexcept
{
    ENGINE_PROFILE_FUNCTION();
    shutdown();
}

Scene* WorldSystem::create_scene(std::string_view name)
{
    ENGINE_PROFILE_ZONE("World.createScene");
    if (!is_initialized() || !can_modify() || scope_write_active_ || scene_ != nullptr ||
        scene_sequence_ == std::numeric_limits<std::uint64_t>::max())
    {
        return nullptr;
    }
    std::unique_ptr<Scene> candidate(new (std::nothrow) Scene(*this, scene_sequence_ + 1));
    if (candidate && candidate->initialize(name) && candidate->publish())
    {
        scene_ = candidate.get();
        scenes_.push_back(std::move(candidate));
        ++scene_sequence_;
        paused_ = false;
    }
    return scene_;
}

ResourceLoader* WorldSystem::resource_loader() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return resources_;
}

Scene* WorldSystem::load_scene(std::string_view path)
{
    ENGINE_PROFILE_ZONE("World.loadScene");
    if (!is_initialized() || !can_modify() || scope_write_active_ || !resources_ ||
        scene_sequence_ == std::numeric_limits<std::uint64_t>::max())
    {
        return nullptr;
    }
    ResourceReference resource;
    if (!resource.acquire(resources_, path))
    {
        return nullptr;
    }
    const auto* data = resource.data<SceneResource>();
    if (!data || !data->info)
    {
        return nullptr;
    }
    std::unique_ptr<Scene> candidate(new (std::nothrow) Scene(*this, scene_sequence_ + 1));
    ecs_detail::ResourceBindingScope scope(*this, resource.handle());
    if (!candidate || !candidate->initialize(data->info->name) || !candidate->restore_data(*data) ||
        !candidate->publish())
    {
        return nullptr;
    }
    candidate->resource_ = resource.detach();
    resources_->set_memory_owner(candidate->resource_,
                                 {ResourceMemoryOwnerKind::scene, world_epoch_, candidate->id()});
    retain_persistent();
    scene_ = candidate.get();
    scenes_.push_back(std::move(candidate));
    ++scene_sequence_;
    paused_ = false;
    return scene_;
}

void WorldSystem::destroy_scene() noexcept
{
    ENGINE_PROFILE_ZONE("World.destroyScenes");
    if (!can_modify() || scope_write_active_)
    {
        return;
    }
    retirement_.clear();
    // Component cleanup can query/release scopes. Remove owners from lookup first;
    // unique_ptr/container destruction must not expose a partially retired scene.
    auto retiring_scopes = std::exchange(scopes_, {});
    auto retiring_scenes = std::exchange(scenes_, {});
    scene_ = nullptr;
    retiring_scopes.clear();
    retiring_scenes.clear();
    paused_ = false;
}

void WorldSystem::retain_persistent()
{
    ENGINE_PROFILE_FUNCTION();
    scene_ = nullptr;
    for (const auto& scene : scenes_)
    {
        scene->retain_persistent();
    }
    std::erase_if(scenes_, [](const auto& scene)
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        return scene->empty();
    });
}

void WorldSystem::unload_scene() noexcept
{
    ENGINE_PROFILE_ZONE("World.unloadScene");
    if (!can_modify() || scope_write_active_)
    {
        return;
    }
    std::erase_if(scopes_, [](const auto& entry)
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        return !entry.second->published;
    });
    retirement_.clear();
    retain_persistent();
    paused_ = false;
}

bool WorldSystem::destroy_scene(std::uint64_t id) noexcept
{
    ENGINE_PROFILE_ZONE("World.destroyScene");
    if (!can_modify() || scope_write_active_)
    {
        return false;
    }
    auto target = std::find_if(scenes_.begin(), scenes_.end(),
                               [id](const auto& item)
                               {
                                   ENGINE_PROFILE_FUNCTION_PROBE();
                                   return item->id() == id;
                               });
    if (target != scenes_.end())
    {
        const auto* owner = target->get();
        std::erase_if(scopes_, [&](const auto& item)
        {
            ENGINE_PROFILE_FUNCTION_PROBE();
            return item.second->destination == id;
        });
        std::erase_if(scenes_,
                      [owner](const auto& item)
                      {
                          ENGINE_PROFILE_FUNCTION_PROBE();
                          return item->allocation_target_ == owner;
                      });
    }
    if (scene_ != nullptr && scene_->id() == id)
    {
        scene_ = nullptr;
        paused_ = false;
    }
    return std::erase_if(scenes_, [id](const auto& scene)
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        return scene->id() == id;
    }) != 0;
}

Scene* WorldSystem::scene() noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return scene_;
}

const Scene* WorldSystem::scene() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return scene_;
}

const std::vector<std::unique_ptr<Scene>>& WorldSystem::scenes() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return scenes_;
}

Node WorldSystem::find_node(std::uint64_t id) const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    if (!world_ || id == 0 || !ecs_is_alive(world_, id))
    {
        return {};
    }
    flecs::world current(world_);
    const auto entity = current.entity(id);
    if (!entity.has<ecs_detail::Published>() || !entity.has<ecs_detail::NodeRuntime>())
    {
        return {};
    }
    return Node(*const_cast<WorldSystem*>(this), world_epoch_, id);
}

ecs_world_t* WorldSystem::native_world() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return world_;
}
std::uint64_t WorldSystem::epoch() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return world_epoch_;
}
bool WorldSystem::can_modify() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return world_ && !ecs_is_stage(world_) && !ecs_stage_is_readonly(world_);
}
void WorldSystem::invalidate_scene_state() noexcept
{
    ENGINE_PROFILE_FUNCTION();
    ++state_revision_;
    if (state_revision_ == 0)
    {
        state_revision_ = 1;
        prepared_revision_ = 0;
    }
}

bool WorldSystem::set_paused(bool paused) noexcept
{
    ENGINE_PROFILE_FUNCTION();
    if (!is_initialized() || scene_ == nullptr)
    {
        return false;
    }
    paused_ = paused;
    return true;
}

bool WorldSystem::paused() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return is_initialized() && scene_ != nullptr && paused_;
}

ComponentRegistry& WorldSystem::component_registry() noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return components_;
}

bool WorldSystem::on_initialize(const EngineConfig&)
{
    ENGINE_PROFILE_ZONE("World.initialize");
    if (restore_config_.nodes_per_step == 0 || !std::isfinite(restore_config_.max_seconds) ||
        restore_config_.max_seconds <= 0.0)
    {
        return false;
    }
    static std::atomic<std::uint64_t> epochs{0};
    auto previous = epochs.load(std::memory_order_relaxed);
    do
    {
        if (previous == std::numeric_limits<std::uint64_t>::max())
        {
            return false;
        }
    } while (!epochs.compare_exchange_weak(previous, previous + 1, std::memory_order_relaxed));
    world_epoch_ = previous + 1;
#if defined(ENGINE_ENABLE_MEMORY_PROFILING)
    enable_flecs_memory_profiling();
#endif
    world_ = ecs_init();
    if (!world_)
    {
        return false;
    }
    components_ = {};
    components_.world_ = world_;
    paused_ = false;
    state_revision_ = 1;
    prepared_revision_ = 0;
    flecs::world current(world_);
    current.component<schema::NodePropertiesT>();
    current.component<schema::SceneInfoT>().add(flecs::OnInstantiate, EcsDontInherit);
    current.component<schema::PrefabInstanceDataT>();
    current.component<ecs_detail::NodeRuntime>();
    current.component<ecs_detail::WorldTransform>();
    current.component<ecs_detail::TransformDrive>();
    current.component<ecs_detail::Published>().add(flecs::OnInstantiate, flecs::Inherit);
    current.component<ecs_detail::Simulating>().add(flecs::OnInstantiate, flecs::Inherit);
    current.component<ecs_detail::ScopeState>().add(flecs::OnInstantiate, EcsDontInherit);
    current.component<ecs_detail::SceneMember>()
        .add(flecs::Exclusive)
        .add(flecs::OnDeleteTarget, flecs::Delete);
    if (!register_scene_components(components_))
    {
        return false;
    }
    current.observer<const schema::NodePropertiesT, const ecs_detail::NodeRuntime>()
        .event(flecs::OnRemove)
        .each(
            [](flecs::entity entity, const schema::NodePropertiesT& properties,
               const ecs_detail::NodeRuntime& state)
            {
                ENGINE_PROFILE_FUNCTION_PROBE();
                if (state.scene)
                {
                    state.scene->unregister_entity(entity.id(), properties.id);
                }
                if (state.scope_scene && state.scope_scene != state.scene)
                {
                    state.scope_scene->unregister_entity(entity.id(), properties.id);
                }
            });
    current.observer<const ecs_detail::NodeRuntime>()
        .event(flecs::OnSet)
        .event<Node>()
        .each(
            [this](flecs::entity entity, const ecs_detail::NodeRuntime& runtime)
            {
                ENGINE_PROFILE_FUNCTION_PROBE();
                if (!runtime.scope_scene || !runtime.scope_scene->published_ ||
                    runtime.scene != scene_)
                {
                    return;
                }
                const auto node = Node(*this, world_epoch_, entity.id());
                for (auto& [key, operation] : scopes_)
                {
                    (void)key;
                    auto& scope = *operation;
                    if (scope.kind != SceneLoadKind::replace_scene || !scope.sealed ||
                        scope.published)
                    {
                        continue;
                    }
                    const auto prepared = scope.prepared_roots.find(node.id());
                    bool changed = false;
                    if (prepared != scope.prepared_roots.end())
                    {
                        auto root = node;
                        while (root.parent())
                        {
                            root = root.parent();
                        }
                        if (prepared->second == std::pair{root.id(), root.persistent()})
                        {
                            continue;
                        }
                        scope.previous_prepared = 0;
                        scope.previous_batch = 0;
                        scope.previous_batch_node = 0;
                        scope.retirement_roots.clear();
                        scope.prepared_roots.clear();
                        scope.progress.completed = scope.mapped;
                        scope.progress.state = SceneWorkState::working;
                        changed = true;
                    }
                    else if (scope.previous_entities.insert(node.id()).second)
                    {
                        scope.previous_nodes.push_back(node);
                        ++scope.progress.total;
                        scope.progress.state = SceneWorkState::working;
                        changed = true;
                    }
                    if (changed)
                    {
                        ++scope.progress.progress_revision;
                        scope.progress.last_progress_ns = static_cast<std::uint64_t>(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
                    }
                }
            });
    ecs_query_desc_t query{};
    query.terms[0].id = current.id<schema::NodePropertiesT>();
    query.terms[1].id = current.id<ecs_detail::NodeRuntime>();
    query.terms[2].id = current.id<ecs_detail::WorldTransform>();
    state_query_ = ecs_query_init(world_, &query);
    return state_query_ != nullptr;
}

bool WorldSystem::on_tick(double delta_time)
{
    ENGINE_PROFILE_ZONE("World.tick");
    if (!can_modify() || !std::isfinite(delta_time) || delta_time < 0.0)
    {
        return false;
    }
    const auto notification_count = geometry_notifications_.size();
    for (std::size_t index = 0; index < notification_count; ++index)
    {
        const auto component = geometry_notifications_[index];
        if (component)
        {
            flecs::world(world_)
                .event<Component>()
                .id<ecs_detail::NodeRuntime>()
                .entity(component.node().id())
                .ctx(component)
                .emit();
        }
    }
    geometry_notifications_.erase(geometry_notifications_.begin(),
                                  geometry_notifications_.begin() + notification_count);
    const auto started = std::chrono::steady_clock::now();
    for (std::size_t item = 0; item < restore_config_.nodes_per_step && !retirement_.empty();
         ++item)
    {
        if (item != 0 &&
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() >=
                restore_config_.max_seconds)
        {
            break;
        }
        const auto [node, expanded] = retirement_.back();
        retirement_.pop_back();
        if (!node)
        {
            continue;
        }
        flecs::world current(world_);
        if (!expanded)
        {
            retirement_.emplace_back(node, true);
            current.entity(node.id()).children(
                [&](flecs::entity child)
                {
                    ENGINE_PROFILE_FUNCTION_PROBE();
                    retirement_.emplace_back(Node(*this, world_epoch_, child.id()), false);
                });
        }
        else
        {
            ecs_delete(world_, node.id());
        }
    }
    std::erase_if(scopes_,
                  [&](const auto& entry)
                  {
                      ENGINE_PROFILE_FUNCTION_PROBE();
                      const auto& scope = *entry.second;
                      return scope.published &&
                             std::none_of(scenes_.begin(), scenes_.end(),
                                          [&](const auto& scene)
                                          {
                                              ENGINE_PROFILE_FUNCTION_PROBE();
                                              return scene.get() == scope.published_scene &&
                                                     (scene.get() == scene_ || !scene->empty());
                                          });
                  });
    std::erase_if(scenes_,
                  [this](const auto& scene)
                  {
                      ENGINE_PROFILE_FUNCTION_PROBE();
                      return scene.get() != scene_ && scene->empty();
                  });
    return refresh_scene_state();
}

bool WorldSystem::refresh_scene_state()
{
    ENGINE_PROFILE_ZONE("World.refreshSceneState");
    if (!world_ || !state_query_)
    {
        return false;
    }
    if (prepared_revision_ == state_revision_)
    {
        return true;
    }
    if (!can_modify() || scope_write_active_)
    {
        return false;
    }
    flecs::world current(world_);
    std::vector<flecs::entity_t> branch;
    auto iterator = ecs_query_iter(world_, state_query_);
    while (ecs_query_next(&iterator))
    {
        for (int32_t row = 0; row < iterator.count; ++row)
        {
            branch.clear();
            auto entity = current.entity(iterator.entities[row]);
            while (entity)
            {
                const auto* state = entity.try_get<ecs_detail::NodeRuntime>();
                if (!state || state->prepared_revision == state_revision_)
                {
                    break;
                }
                branch.push_back(entity.id());
                entity = entity.parent();
            }
            for (auto item = branch.rbegin(); item != branch.rend(); ++item)
            {
                auto node = current.entity(*item);
                auto* state = node.try_get_mut<ecs_detail::NodeRuntime>();
                auto* matrix = node.try_get_mut<ecs_detail::WorldTransform>();
                const auto* properties = node.try_get<schema::NodePropertiesT>();
                if (!state || !matrix || !properties)
                {
                    continue;
                }
                const auto parent = node.parent();
                const auto* parent_state =
                    parent ? parent.try_get<ecs_detail::NodeRuntime>() : nullptr;
                const auto* parent_matrix =
                    parent ? parent.try_get<ecs_detail::WorldTransform>() : nullptr;
                state->active =
                    properties->enabled && (!parent || (parent_state && parent_state->active));
                state->visible_in_hierarchy =
                    properties->visible &&
                    (!parent || (parent_state && parent_state->visible_in_hierarchy));
                const auto* transform =
                    node.try_get<ecs_detail::ComponentStorage<schema::TransformDataT>>();
                *matrix = {};
                matrix->valid = transform && (!parent || (parent_matrix && parent_matrix->valid));
                if (matrix->valid)
                {
                    const auto& value = transform->data;
                    constexpr double radians = std::numbers::pi / 180.0;
                    const double sx = std::sin(value.rotation.x() * radians);
                    const double cx = std::cos(value.rotation.x() * radians);
                    const double sy = std::sin(value.rotation.y() * radians);
                    const double cy = std::cos(value.rotation.y() * radians);
                    const double sz = std::sin(value.rotation.z() * radians);
                    const double cz = std::cos(value.rotation.z() * radians);
                    std::array<double, 16> local{cz * cy * value.scale.x(),
                                                 sz * cy * value.scale.x(),
                                                 -sy * value.scale.x(),
                                                 0,
                                                 (cz * sy * sx - sz * cx) * value.scale.y(),
                                                 (sz * sy * sx + cz * cx) * value.scale.y(),
                                                 cy * sx * value.scale.y(),
                                                 0,
                                                 (cz * sy * cx + sz * sx) * value.scale.z(),
                                                 (sz * sy * cx - cz * sx) * value.scale.z(),
                                                 cy * cx * value.scale.z(),
                                                 0,
                                                 value.position.x(),
                                                 value.position.y(),
                                                 value.position.z(),
                                                 1};
                    const auto* drive = node.try_get<ecs_detail::TransformDrive>();
                    if (drive && drive->active && state->active && transform->state.flags.enabled &&
                        drive->validate &&
                        drive->validate(Node(*this, world_epoch_, node.id()),
                                        drive->owner_instance_id))
                    {
                        std::copy(drive->local.begin(), drive->local.end(), local.begin());
                    }
                    std::array<double, 16> result = local;
                    if (parent)
                    {
                        result = {};
                        for (std::size_t column = 0; column < 4; ++column)
                        {
                            for (std::size_t line = 0; line < 4; ++line)
                            {
                                for (std::size_t index = 0; index < 4; ++index)
                                {
                                    result[column * 4 + line] +=
                                        parent_matrix->matrix[index * 4 + line] *
                                        local[column * 4 + index];
                                }
                            }
                        }
                    }
                    for (std::size_t index = 0; index < 16; ++index)
                    {
                        if (!std::isfinite(result[index]))
                        {
                            matrix->valid = false;
                            break;
                        }
                        matrix->matrix[index] = result[index];
                    }
                    if (!matrix->valid)
                    {
                        matrix->matrix = ecs_detail::WorldTransform{}.matrix;
                    }
                }
                state->prepared_revision = state_revision_;
            }
        }
    }
    prepared_revision_ = state_revision_;
    return true;
}

void WorldSystem::on_shutdown() noexcept
{
    ENGINE_PROFILE_ZONE("World.shutdown");
    destroy_scene();
    if (state_query_)
    {
        ecs_query_fini(state_query_);
        state_query_ = nullptr;
    }
    if (world_)
    {
        if (ecs_fini(world_) != 0)
        {
            std::fputs("Cannot finish the ECS world cleanly\n", stderr);
        }
        world_ = nullptr;
    }
    components_ = {};
}
} // namespace engine
