#pragma once
#include "plugins/editor/terrain/terrain_generator.hpp"
#include "plugins/runtime/terrain/terrain_mesher.hpp"
#include "runtime/system/system.hpp"
#include <cstdint>
#include <map>
#include <memory>
#include <string>

namespace engine
{
class TaskSystem;
namespace editor
{
class EditorEnvironment;
class EditorEdit;
} // namespace editor
} // namespace engine
namespace engine::plugins::terrain
{
struct TerrainAuthoringId
{
    std::uint64_t session = 0, sequence = 0;
    auto operator<=>(const TerrainAuthoringId&) const = default;
};
enum class TerrainAuthoringState : std::uint8_t
{
    queued,
    preparing,
    ready_to_publish,
    published,
    failed,
    cancelled
};
struct TerrainAuthoringSpec
{
    std::string path;
    std::uint64_t expected_source_revision = 0;
    // Full source recipe, including ordered sculpt/paint edits and roads. Zero revision creates.
    schema::TerrainSourceDataT source;
};
struct TerrainAuthoringSnapshot
{
    TerrainAuthoringId id;
    TerrainAuthoringState state = TerrainAuthoringState::queued;
    std::uint64_t completed = 0, total = 0, source_revision = 0;
    std::string path, guid, content_hash, error_code, error;
    bool can_cancel = true, can_publish = false;
};
struct TerrainAuthoringJob;
class TerrainAuthoringSystem final : public System
{
public:
    TerrainAuthoringSystem(editor::EditorEnvironment& environment, TaskSystem& tasks,
                           std::unique_ptr<TerrainGenerator> generator,
                           std::unique_ptr<TerrainMesher> mesher);
    ~TerrainAuthoringSystem() noexcept override;
    [[nodiscard]] bool start(const TerrainAuthoringSpec& spec, TerrainAuthoringId& output);
    [[nodiscard]] bool status(TerrainAuthoringId id, TerrainAuthoringSnapshot& output) const;
    [[nodiscard]] bool cancel(TerrainAuthoringId id);
    [[nodiscard]] bool make_edit(TerrainAuthoringId id,
                                 std::unique_ptr<editor::EditorEdit>& output);
    [[nodiscard]] bool release(TerrainAuthoringId id);

protected:
    [[nodiscard]] bool on_initialize(const EngineConfig& config) override;
    [[nodiscard]] bool on_tick(double delta_time) override;
    void on_shutdown() noexcept override;

private:
    editor::EditorEnvironment& environment_;
    TaskSystem& tasks_;
    std::unique_ptr<TerrainGenerator> generator_;
    std::unique_ptr<TerrainMesher> mesher_;
    std::map<std::uint64_t, std::shared_ptr<TerrainAuthoringJob>> jobs_;
    std::uint64_t session_ = 0, sequence_ = 0;
};
} // namespace engine::plugins::terrain
