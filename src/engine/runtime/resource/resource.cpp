#include "runtime/resource/resource.hpp"
#include "runtime/performance/profiling.hpp"

namespace engine
{
Resource::Resource(schema::AssetEntryT info) : info_(std::move(info))
{
    ENGINE_PROFILE_FUNCTION();
}
Resource::~Resource() noexcept = default;
const schema::AssetEntryT& Resource::info() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return info_;
}
std::uint64_t Resource::generation() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return generation_;
}
std::optional<std::uint64_t> Resource::cpu_payload_capacity() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return std::nullopt;
}
} // namespace engine
