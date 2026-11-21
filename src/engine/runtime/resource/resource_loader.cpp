#include "runtime/resource/resource_loader.hpp"
#include "runtime/performance/profiling.hpp"

#include <atomic>
#include <limits>

namespace engine
{
namespace
{
std::uint64_t allocate_owner() noexcept
{
    ENGINE_PROFILE_FUNCTION();
    static std::atomic<std::uint64_t> sequence{0};
    auto value = sequence.load(std::memory_order_relaxed);
    while (value != std::numeric_limits<std::uint64_t>::max())
    {
        if (sequence.compare_exchange_weak(value, value + 1, std::memory_order_relaxed))
        {
            return value + 1;
        }
    }
    return 0;
}
} // namespace

ResourceLoader::ResourceLoader() noexcept : owner_id_(allocate_owner())
{
    ENGINE_PROFILE_FUNCTION();
}
std::uint64_t ResourceLoader::owner_id() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return owner_id_;
}
void ResourceLoader::set_memory_owner(const ResourceHandle&, ResourceMemoryOwner) noexcept
{
    ENGINE_PROFILE_FUNCTION();
}

bool ResourceLoader::memory_snapshot(const ResourceMemoryQuery&, ResourceMemorySnapshot& output) const
{
    ENGINE_PROFILE_FUNCTION();
    output = {};
    return false;
}
} // namespace engine
