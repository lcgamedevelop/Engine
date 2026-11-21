#pragma once

#include "runtime/schema/asset_catalog_generated.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace engine
{
enum class ResourceMemoryOwnerKind : std::uint8_t
{
    unspecified,
    scene,
    scene_scope,
    scene_load
};

struct ResourceMemoryOwner
{
    ResourceMemoryOwnerKind kind = ResourceMemoryOwnerKind::unspecified;
    std::uint64_t domain = 0;
    std::uint64_t id = 0;
};

enum class ResourceMemoryPhase : std::uint8_t
{
    queued,
    reading,
    decoded,
    ready,
    failed,
    retiring
};

enum class ResourceMemoryHolderKind : std::uint8_t
{
    acquisition,
    dependency,
    load_request
};

struct ResourceMemoryHolder
{
    ResourceMemoryHolderKind kind = ResourceMemoryHolderKind::acquisition;
    std::uint64_t id = 0;
    ResourceMemoryOwner owner;
    std::string guid;
    bool pending_release = false;
};

struct ResourceMemoryEntry
{
    std::uint64_t record_id = 0;
    std::uint64_t catalog_generation = 0;
    std::string guid;
    std::string path;
    schema::AssetKind kind = schema::AssetKind::source;
    ResourceMemoryPhase phase = ResourceMemoryPhase::queued;
    std::uint64_t stored_payload_bytes = 0;
    std::optional<std::uint64_t> cpu_payload_bytes;
    bool has_payload = false;
    bool holders_truncated = false;
    std::uint64_t acquisitions = 0;
    std::uint64_t dependency_references = 0;
    std::uint64_t request_references = 0;
    std::vector<ResourceMemoryHolder> holders;
};

struct ResourceMemoryQuery
{
    static constexpr std::uint32_t maximum_items = 64;
    static constexpr std::uint32_t maximum_holders_per_item = 32;

    std::string_view guid;
    std::uint32_t limit = 32;
};

struct ResourceMemorySnapshot
{
    std::uint64_t loader_id = 0;
    std::uint64_t session_started_ns = 0;
    std::uint64_t sequence = 0;
    std::uint64_t timestamp_ns = 0;
    std::uint64_t catalog_generation = 0;
    std::uint64_t measured_cpu_payload_bytes = 0;
    std::uint64_t measured_payloads = 0;
    std::uint64_t unmeasured_payloads = 0;
    std::uint64_t matching_entries = 0;
    std::vector<ResourceMemoryEntry> entries;
};
} // namespace engine
