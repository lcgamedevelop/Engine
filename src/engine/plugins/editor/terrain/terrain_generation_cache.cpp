#include "plugins/editor/terrain/terrain_generation_cache.hpp"
#include "editor/asset_pipeline/asset_io.hpp"
#include "plugins/editor/terrain/terrain_source.hpp"
#include "plugins/runtime/terrain/terrain_samples.hpp"
#include "runtime/utils/path_utils.hpp"
#include <SDL3/SDL_filesystem.h>
#include <algorithm>
#include <cmath>
#include <flatbuffers/flatbuffers.h>

namespace engine::plugins::terrain
{
bool generate_terrain_block(const TerrainGenerationInput& input, const TerrainGenerator& generator,
                            const std::filesystem::path& cache, schema::TerrainBlockDataT& output,
                            std::string& error)
{
    namespace io = editor::asset_pipeline;
    namespace fs = std::filesystem;
    output = {};
    if (generator.fingerprint().empty())
    {
        error = "Terrain generation requires an implementation fingerprint";
        return false;
    }
    if (std::any_of(input.recipe.stamps.begin(), input.recipe.stamps.end(),
                    [](const auto& brush) { return !brush; }) ||
        std::any_of(input.recipe.roads.begin(), input.recipe.roads.end(),
                    [](const auto& road) { return !road; }))
    {
        error = "Terrain generation requires complete stamps and roads";
        return false;
    }
    auto local = input;
    auto& recipe = local.recipe;
    const auto spacing = std::ldexp(recipe.voxel_size, input.key.lod());
    const double edge = spacing * recipe.block_cells;
    const Vec3 low{recipe.origin.x() + input.key.x() * edge,
                   recipe.origin.y() + input.key.y() * edge,
                   recipe.origin.z() + input.key.z() * edge};
    const double margin =
        recipe.voxel_size *
        (1 + std::count_if(recipe.stamps.begin(), recipe.stamps.end(), [](const auto& brush)
                           { return brush->operation == schema::TerrainBrushOperation::smooth; }));
    const Bounds3 bounds{
        {low.x() - margin, low.y() - margin, low.z() - margin},
        {low.x() + edge + margin, low.y() + edge + margin, low.z() + edge + margin}};
    std::erase_if(recipe.stamps,
                  [&](const auto& brush)
                  {
                      const auto extent =
                          brush->shape == schema::TerrainBrushShape::sphere
                              ? Vec3{brush->extent.x(), brush->extent.x(), brush->extent.x()}
                              : brush->extent;
                      const auto p = brush->center;
                      return !terrain_detail::intersects(
                          bounds, {{p.x() - extent.x(), p.y() - extent.y(), p.z() - extent.z()},
                                   {p.x() + extent.x(), p.y() + extent.y(), p.z() + extent.z()}});
                  });
    std::erase_if(recipe.roads,
                  [&](const auto& road)
                  {
                      const double radius = road->width * .5 + road->falloff;
                      for (std::size_t i = 1; i < road->points.size(); ++i)
                      {
                          const auto a = road->points[i - 1], b = road->points[i];
                          if (std::min(a.x(), b.x()) - radius <= bounds.maximum().x() &&
                              std::max(a.x(), b.x()) + radius >= bounds.minimum().x() &&
                              std::min(a.z(), b.z()) - radius <= bounds.maximum().z() &&
                              std::max(a.z(), b.z()) + radius >= bounds.minimum().z())
                          {
                              return false;
                          }
                      }
                      return true;
                  });
    flatbuffers::FlatBufferBuilder fingerprint;
    fingerprint.Finish(schema::TerrainGenerationSpec::Pack(fingerprint, &recipe));
    const auto digest = io::hash_bytes(
        std::as_bytes(std::span(fingerprint.GetBufferPointer(), fingerprint.GetSize())));
    const auto path = cache / terrain_generation_revision /
                      io::hash_text(std::string(generator.fingerprint())) / digest /
                      terrain_region_name(input.key);
    std::vector<std::byte> bytes;
    std::error_code ec;
    if (fs::exists(path, ec))
    {
        if (!io::read_bytes(path, bytes, error))
        {
            return false;
        }
        flatbuffers::Verifier verifier(reinterpret_cast<const std::uint8_t*>(bytes.data()),
                                       bytes.size());
        if (!schema::VerifyTerrainBlockDataBuffer(verifier))
        {
            error = "Invalid terrain block cache";
            return false;
        }
        auto block = schema::UnPackTerrainBlockData(bytes.data());
        if (block->schema_version != 2 || block->key != input.key ||
            block->density.size() !=
                (block->uniform
                     ? 1
                     : std::size_t(recipe.block_cells) * recipe.block_cells * recipe.block_cells) ||
            block->layers.size() != block->density.size() ||
            !std::all_of(block->density.begin(), block->density.end(),
                         [](float v) { return std::isfinite(v); }) ||
            !std::all_of(block->layers.begin(), block->layers.end(), [&](const auto& v)
                         { return terrain_detail::layer_valid(v, recipe.material_layers.size()); }))
        {
            error = "Terrain cached block does not match its recipe";
            return false;
        }
        output = std::move(*block);
        return true;
    }
    if (ec)
    {
        error = "Cannot inspect terrain block cache";
        return false;
    }
    if (!generator.generate(local, output, error))
    {
        return false;
    }
    flatbuffers::FlatBufferBuilder builder;
    schema::FinishTerrainBlockDataBuffer(builder, schema::TerrainBlockData::Pack(builder, &output));
    fs::create_directories(path.parent_path(), ec);
    if (ec)
    {
        error = "Cannot create terrain block cache";
        return false;
    }
    auto pending = path;
    pending += ".pending-" + io::new_guid();
    if (!io::write_bytes(pending,
                         std::as_bytes(std::span(builder.GetBufferPointer(), builder.GetSize())),
                         error))
    {
        return false;
    }
    if (!SDL_RenamePath(PathUtils::to_utf8(pending).c_str(), PathUtils::to_utf8(path).c_str()))
    {
        error = "Cannot publish terrain block cache";
        fs::remove(pending, ec);
        return false;
    }
    return true;
}
} // namespace engine::plugins::terrain
