#pragma once

#include "plugins/runtime/terrain/terrain_volume.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace engine::plugins::terrain::terrain_detail
{
[[nodiscard]] bool materialize_block(const TerrainReadSnapshot& input, TerrainRegionKey key,
                                     schema::TerrainBlockDataT& output);
[[nodiscard]] bool propagate_lods(const TerrainReadSnapshot& input, TerrainDelta& candidate,
                                  std::string& error);
[[nodiscard]] bool finite(const Vec3& value) noexcept;
[[nodiscard]] bool valid_bounds(const Bounds3& value) noexcept;
[[nodiscard]] bool intersects(const Bounds3& a, const Bounds3& b) noexcept;
[[nodiscard]] Bounds3 region_bounds(const schema::TerrainAssetDataT& source,
                                    TerrainRegionKey key) noexcept;
[[nodiscard]] const schema::TerrainRegionEntryT* entry(const schema::TerrainAssetDataT& source,
                                                       TerrainRegionKey key) noexcept;
[[nodiscard]] bool enumerate(const schema::TerrainAssetDataT& source, const Bounds3& bounds,
                             std::uint8_t lod, std::size_t limit,
                             std::vector<TerrainRegionKey>& output);
[[nodiscard]] bool layer_valid(const schema::TerrainLayerSample& value,
                               std::size_t layer_count) noexcept;
[[nodiscard]] schema::TerrainLayerSample blend_layers(const schema::TerrainLayerSample& a,
                                                      const schema::TerrainLayerSample& b,
                                                      double weight);
// Integer coordinates are measured in samples at the requested LOD. Outside declared bounds is air.
[[nodiscard]] bool sample(const TerrainReadSnapshot& input, std::int64_t x, std::int64_t y,
                          std::int64_t z, std::uint8_t lod, float& density,
                          schema::TerrainLayerSample* layer = nullptr) noexcept;
[[nodiscard]] bool resident(const TerrainReadSnapshot& input, TerrainRegionKey key) noexcept;
} // namespace engine::plugins::terrain::terrain_detail
