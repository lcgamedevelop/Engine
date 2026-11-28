#pragma once

#include "runtime/resource/resource.hpp"
#include "runtime/schema/mesh_generated.h"

namespace engine
{
namespace resource_detail
{
template <> struct Payload<schema::AssetKind::mesh>
{
    using Type = schema::MeshDataT;
    [[nodiscard]] static std::uint64_t cpu_payload_capacity(const Type& value) noexcept;
};
} // namespace resource_detail
using MeshResource = ResourceValue<schema::MeshDataT, schema::AssetKind::mesh>;
} // namespace engine
