#pragma once

#include "runtime/scene/scene_types.hpp"
#include "runtime/schema/terrain_block_generated.h"

#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace engine::plugins::terrain
{
using TerrainRegionKey = schema::TerrainRegionKey;

struct TerrainRegionLess
{
    [[nodiscard]] bool operator()(const TerrainRegionKey& a,
                                  const TerrainRegionKey& b) const noexcept;
};

struct TerrainRegionQuery
{
    Bounds3 bounds;
    std::uint8_t lod = 0;
    std::uint32_t limit = 256;
    std::string cursor;
};

struct TerrainEditSpec
{
    // Zero accepts the current revision at the time this operation starts.
    std::uint64_t expected_revision = 0;
    std::vector<schema::TerrainBrushDataT> brushes;
};

using TerrainBlocks =
    std::map<TerrainRegionKey, std::shared_ptr<const schema::TerrainBlockDataT>, TerrainRegionLess>;

struct TerrainReadSnapshot
{
    std::uint64_t source_revision = 0;
    std::shared_ptr<const schema::TerrainAssetDataT> source;
    TerrainBlocks blocks;
    std::vector<TerrainRegionKey> regions;
};

struct TerrainDelta
{
    std::uint64_t base_revision = 0;
    Bounds3 dirty_bounds;
    std::shared_ptr<const schema::TerrainAssetDataT> source;
    TerrainBlocks blocks;
    std::vector<TerrainRegionKey> changed;
};

// Pure immutable region storage and editing; no scene, resource loader or device access.
class TerrainVolume final
{
public:
    [[nodiscard]] static bool validate(const schema::TerrainAssetDataT& source, std::string& error);
    [[nodiscard]] bool initialize(const schema::TerrainAssetDataT& source, std::string& error);
    [[nodiscard]] bool stage_regions(std::span<const schema::TerrainBlockDataT> blocks,
                                     std::span<const TerrainRegionKey> retire, TerrainDelta& output,
                                     std::string& error) const;
    [[nodiscard]] bool read(const TerrainRegionQuery& query, TerrainReadSnapshot& output) const;
    [[nodiscard]] static bool stage_edit(const TerrainReadSnapshot& input,
                                         const TerrainEditSpec& edit, TerrainDelta& output,
                                         std::string& error);
    [[nodiscard]] bool validate_delta(const TerrainDelta& delta) const noexcept;
    void publish(TerrainDelta&& delta) noexcept;

private:
    std::shared_ptr<const schema::TerrainAssetDataT> source_;
    TerrainBlocks blocks_;
    std::uint64_t revision_ = 0;
};
} // namespace engine::plugins::terrain
