#pragma once

#include "runtime/resource/virtual_file_system.hpp"
#include "runtime/schema/terrain_state_generated.h"

#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace engine::plugins::terrain
{
// Serialized by its owner. Stage/read run in the shared task domain; publish only replaces a
// manifest.
class TerrainStateStore final
{
public:
    explicit TerrainStateStore(std::filesystem::path persistent_root);
    ~TerrainStateStore() noexcept;
    TerrainStateStore(const TerrainStateStore&) = delete;
    TerrainStateStore& operator=(const TerrainStateStore&) = delete;
    [[nodiscard]] bool stage(const schema::TerrainCheckpointDataT& snapshot,
                             std::span<const schema::TerrainBlockDataT> blocks,
                             std::string_view slot, std::string& error);
    [[nodiscard]] bool publish(std::string& error);
    [[nodiscard]] bool read(std::string_view slot, schema::TerrainCheckpointDataT& output,
                            std::string& error) const;
    [[nodiscard]] bool read_block(std::string_view slot, std::string_view payload_key,
                                  schema::TerrainBlockDataT& output, std::string& error) const;
    void cancel() noexcept;

private:
    std::filesystem::path root_;
    std::filesystem::path staged_;
    std::filesystem::path manifest_;
    std::string previous_hash_;
};
} // namespace engine::plugins::terrain
