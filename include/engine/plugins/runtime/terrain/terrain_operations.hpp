#pragma once

#include "plugins/runtime/terrain/terrain_regions.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace engine::plugins::terrain
{
struct TerrainOperationId
{
    std::uint64_t world_epoch = 0;
    std::uint64_t node_id = 0;
    std::uint64_t component_instance = 0;
    std::uint64_t sequence = 0;
    [[nodiscard]] bool operator==(const TerrainOperationId&) const noexcept = default;
};

struct TerrainPinId
{
    std::uint64_t world_epoch = 0;
    std::uint64_t node_id = 0;
    std::uint64_t component_instance = 0;
    std::uint64_t sequence = 0;
    [[nodiscard]] bool operator==(const TerrainPinId&) const noexcept = default;
};

enum class TerrainOperationState : std::uint8_t
{
    queued,
    waiting,
    preparing,
    ready,
    published,
    completed,
    cancelled,
    failed
};

struct TerrainOperationSnapshot
{
    TerrainOperationId id;
    TerrainOperationState state = TerrainOperationState::queued;
    std::vector<SceneWorkProgress> stages;
    std::uint64_t base_revision = 0;
    std::uint64_t published_revision = 0;
    std::uint64_t content_revision = 0;
    Bounds3 dirty_bounds;
    std::uint32_t created_fragments = 0;
    std::uint32_t retired_fragments = 0;
    std::string checkpoint_id;
    std::uint64_t captured_revision = 0;
    std::string error_code;
    std::string error;
};

struct TerrainRegionSnapshot
{
    std::uint64_t revision = 0;
    std::uint64_t settings_revision = 0;
    std::uint64_t fragment_count = 0;
    std::vector<TerrainRegionStatus> regions;
    std::string next_cursor;
};

struct TerrainCollapseSpec
{
    Bounds3 bounds;
    std::uint64_t expected_revision = 0;
    std::uint32_t min_samples = 4;
    std::uint32_t fragment_cells = 8;
    std::uint32_t max_fragments = 32;
    float density = 1600;
    std::uint32_t connectivity_block_limit = 256;
};
struct TerrainResetSpec
{
    Bounds3 bounds;
    std::uint64_t expected_revision = 0;
};
struct TerrainRetireSpec
{
    std::uint32_t max_count = 32;
    double max_age_seconds = 0;
    std::uint64_t expected_revision = 0;
};
struct TerrainSaveSpec
{
    std::string slot;
    std::string checkpoint_id;
    std::uint64_t expected_revision = 0;
};
struct TerrainLoadSpec
{
    std::string slot;
    std::string checkpoint_id;
    std::string expected_base_hash;
};
} // namespace engine::plugins::terrain
