#pragma once

#include "runtime/performance/function_profile.hpp"

#include "runtime/resource/resource_reference.hpp"
#include "runtime/scene/node.hpp"
#include "runtime/world/world_system.hpp"

#include <string>

namespace engine::ecs_detail
{
struct ResourceBindingScope
{
    ResourceBindingScope(WorldSystem& world, ResourceHandle owner) noexcept
        : world(world), previous(world.binding_owner_)
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        world.binding_owner_ = owner;
    }
    ~ResourceBindingScope() noexcept
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        world.binding_owner_ = previous;
    }
    ResourceBindingScope(const ResourceBindingScope&) = delete;
    ResourceBindingScope& operator=(const ResourceBindingScope&) = delete;
    static ResourceHandle owner(const WorldSystem& world) noexcept
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        return world.binding_owner_;
    }
    WorldSystem& world;
    ResourceHandle previous;
};

// 只在指定已加载闭包中按路径或 GUID 获取，不查看当前目录。
inline bool acquire_dependency(ResourceLoader* loader, ResourceHandle owner, std::string_view input,
                               ResourceReference& output)
{
    ENGINE_PROFILE_FUNCTION_PROBE();
    if (!loader)
    {
        return false;
    }
    const Resource* root = nullptr;
    if (loader->get(owner, root) != ResourceError::none)
    {
        return false;
    }
    if (root->info().guid == input || root->info().path == input)
    {
        return output.retain(loader, owner);
    }
    return output.dependency(loader, owner, input);
}

inline bool acquire_resource(Node node, std::string_view input, ResourceReference& output)
{
    ENGINE_PROFILE_FUNCTION_PROBE();
    auto* world = node.world();
    if (!node || !world)
    {
        return false;
    }
    const auto owner = ResourceBindingScope::owner(*world);
    return owner.id ? acquire_dependency(world->resource_loader(), owner, input, output)
                    : output.acquire(world->resource_loader(), input);
}

inline bool resource_guid(Node node, std::string_view path, std::string& output)
{
    ENGINE_PROFILE_FUNCTION_PROBE();
    output.clear();
    ResourceReference value;
    if (!acquire_resource(node, path, value))
    {
        return false;
    }
    output = value->info().guid;
    return true;
}
inline bool resource_path(Node node, std::string_view guid, std::string& output)
{
    ENGINE_PROFILE_FUNCTION_PROBE();
    output.clear();
    ResourceReference value;
    if (!acquire_resource(node, guid, value))
    {
        return false;
    }
    output = value->info().path;
    return true;
}
} // namespace engine::ecs_detail
