#pragma once

#include "runtime/performance/function_profile.hpp"

#include "runtime/resource/resource.hpp"
#include "runtime/schema/texture_generated.h"

namespace engine
{
namespace resource_detail
{
template <> struct Payload<schema::AssetKind::texture>
{
    using Type = schema::TextureDataT;
    [[nodiscard]] static std::uint64_t cpu_payload_capacity(const Type& value) noexcept
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        return value.data.capacity() + value.chunks.capacity() * sizeof(schema::TextureChunk);
    }
};
} // namespace resource_detail
using TextureResource = ResourceValue<schema::TextureDataT, schema::AssetKind::texture>;
} // namespace engine
