#include "plugins/editor/terrain/terrain_authoring_system.hpp"
#include "editor/asset_pipeline/asset_io.hpp"
#include "editor/asset_pipeline/asset_metadata.hpp"
#include "editor/asset_pipeline/project_lock.hpp"
#include "editor/commands/editor_edit.hpp"
#include "editor/editor_environment.hpp"
#include "editor/file_text.hpp"
#include "plugins/editor/terrain/terrain_files.hpp"
#include "plugins/editor/terrain/terrain_generation_cache.hpp"
#include "plugins/editor/terrain/terrain_source.hpp"
#include "plugins/runtime/terrain/terrain_samples.hpp"
#include "runtime/app/app.hpp"
#include "runtime/task/task_system.hpp"
#include "runtime/utils/path_utils.hpp"
#include <SDL3/SDL_filesystem.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <flatbuffers/flatbuffers.h>
#include <fstream>
#include <future>
#include <limits>
#include <nlohmann/json.hpp>
#include <toml++/toml.hpp>

namespace engine::plugins::terrain
{
namespace fs = std::filesystem;
namespace io = editor::asset_pipeline;
struct TerrainAuthoringJob
{
    TerrainAuthoringSpec spec;
    TerrainAuthoringSnapshot snapshot;
    fs::path project, destination, workspace;
    std::string before, after, metadata;
    std::shared_ptr<std::atomic<bool>> cancelled = std::make_shared<std::atomic<bool>>(false);
    std::atomic<std::uint64_t> completed = 0, total = 0;
    std::future<bool> future;
    bool existed = false, edit_issued = false, source_already_published = false;
};
namespace
{
bool retain_import_metadata(TerrainAuthoringJob& job, std::string current, std::string& error)
{
    auto before = toml::parse(job.metadata);
    auto after = toml::parse(current);
    if (!before || !after)
    {
        error = "Invalid terrain metadata";
        return false;
    }
    io::ImportAsset asset;
    asset.path = job.spec.path;
    asset.source = job.destination;
    asset.guid = job.snapshot.guid;
    asset.importer_id = "terrain";
    std::vector<io::ImportAsset> old_ids, new_ids;
    auto old_text = job.metadata;
    if (!io::update_subresource_meta(asset, {}, {}, {}, old_text, old_ids, error) ||
        !io::update_subresource_meta(asset, {}, {}, {}, current, new_ids, error))
    {
        return false;
    }
    before.table().erase("subresources");
    after.table().erase("subresources");
    if (before.table() != after.table())
    {
        error = "Terrain meta changed outside this edit";
        return false;
    }
    for (const auto& old : old_ids)
    {
        const auto found = std::find_if(new_ids.begin(), new_ids.end(), [&](const auto& value)
                                        { return value.subresource_key == old.subresource_key; });
        if (found == new_ids.end() || found->guid != old.guid || found->kind != old.kind)
        {
            error = "Terrain subresource identity changed outside this edit";
            return false;
        }
    }
    // Import can append stable virtual-resource identities. Preserve these across undo/redo.
    job.metadata = std::move(current);
    return true;
}
bool atomic_text(const fs::path& destination, const std::string& text, std::string& error)
{
    std::error_code ec;
    fs::create_directories(destination.parent_path(), ec);
    if (ec)
    {
        error = "Cannot create terrain output directory: " + ec.message();
        return false;
    }
    auto pending = destination;
    pending += ".pending-" + io::new_guid() + ".meta";
    if (!io::write_text(pending, text, error))
    {
        return false;
    }
    if (!SDL_RenamePath(PathUtils::to_utf8(pending).c_str(),
                        PathUtils::to_utf8(destination).c_str()))
    {
        error = "Cannot publish terrain source";
        std::error_code ignored;
        fs::remove(pending, ignored);
        return false;
    }
    return true;
}
bool prepare_job(TerrainAuthoringJob& job, const TerrainGenerator& generator,
                 const TerrainMesher& mesher)
{
    std::string& error = job.snapshot.error;
    std::error_code ec;
    if (job.spec.expected_source_revision >= INT64_MAX ||
        job.spec.source.revision != job.spec.expected_source_revision + 1 ||
        !write_terrain_source(job.spec.source, job.after, error))
    {
        return false;
    }
    job.snapshot.content_hash = io::hash_text(job.after);
    job.snapshot.source_revision = job.spec.source.revision;
    job.workspace = job.project / "cache/terrain-authoring/transactions" /
                    io::hash_text(job.spec.path + "\n" + job.after);
    if (!terrain_file_path(job.project, "assets", job.spec.path, ".terrain", job.destination,
                           error))
    {
        return false;
    }
    job.existed = fs::exists(job.destination, ec);
    if (ec)
    {
        error = "Cannot inspect terrain source";
        return false;
    }
    if (job.existed)
    {
        schema::TerrainSourceDataT before;
        if (!editor::file_detail::read_text(job.destination, job.before) ||
            !read_terrain_source(job.before, before, error) ||
            (before.revision != job.spec.expected_source_revision && job.before != job.after))
        {
            error = "Terrain source revision changed";
            return false;
        }
        auto meta = job.destination;
        meta += ".meta";
        if (!editor::file_detail::read_text(meta, job.metadata))
        {
            error = "Terrain meta is missing";
            return false;
        }
        const auto parsed = toml::parse(job.metadata);
        if (!parsed || !(job.snapshot.guid = parsed.table()["guid"].value_or(std::string{})).size())
        {
            error = "Invalid terrain source meta";
            return false;
        }
        if (job.before == job.after)
        {
            job.source_already_published = true;
            return true;
        }
    }
    else
    {
        if (job.spec.expected_source_revision != 0)
        {
            error = "Terrain source does not exist";
            return false;
        }
        job.snapshot.guid = io::new_guid();
        job.metadata =
            "guid = \"" + job.snapshot.guid + "\"\nimporter = \"terrain\"\nmeta_version = 1\n";
    }
    std::vector<TerrainRegionKey> keys;
    if (!terrain_source_keys(job.spec.source, keys, error))
    {
        return false;
    }
    job.total = keys.size();
    job.snapshot.content_hash = io::hash_text(job.after);
    job.snapshot.source_revision = job.spec.source.revision;
    fs::create_directories(job.workspace, ec);
    if (ec)
    {
        error = "Cannot create terrain job workspace";
        return false;
    }
    TerrainGenerationInput input;
    input.recipe = *job.spec.source.recipe;
    input.source_revision = job.spec.source.revision;
    for (const auto& edit : job.spec.source.edits)
    {
        input.recipe.stamps.push_back(std::make_unique<schema::TerrainBrushDataT>(*edit));
    }
    // All preparation stays in the existing editor task domain. The importer reuses these
    // immutable, content-addressed blocks; publication itself never generates volume data.
    const auto artifact_root =
        job.project / "cache/terrain-authoring/generated" /
        (std::string(terrain_generation_revision) + "-" +
         io::hash_text(std::string(generator.fingerprint())) + "-" + job.snapshot.content_hash);
    std::string journal_text, complete;
    if (editor::file_detail::read_text(job.workspace / "prepared.json", journal_text) &&
        editor::file_detail::read_text(artifact_root / "complete", complete) &&
        complete == job.snapshot.content_hash)
    {
        const auto saved = nlohmann::json::parse(journal_text, nullptr, false);
        if (saved.is_object() && saved.contains("version") &&
            saved["version"].is_number_integer() && saved.contains("path") &&
            saved["path"].is_string() && saved.contains("before") && saved["before"].is_string() &&
            saved.contains("after") && saved["after"].is_string() && saved.contains("existed") &&
            saved["existed"].is_boolean() && saved.value("version", 0) == 1 &&
            saved.value("path", std::string{}) == job.spec.path &&
            saved.value("before", std::string{}) == job.before &&
            saved.value("after", std::string{}) == job.after &&
            saved.value("existed", !job.existed) == job.existed && saved.contains("meta") &&
            saved.contains("generator") && saved["generator"].is_string() &&
            saved.value("generator", std::string{}) == generator.fingerprint() &&
            saved["meta"].is_string() && saved.contains("guid") && saved["guid"].is_string())
        {
            const auto guid = saved["guid"].get<std::string>();
            const auto meta = toml::parse(saved["meta"].get<std::string>());
            if (GuidMap::valid_guid(guid) && meta &&
                meta.table()["guid"].value_or(std::string{}) == guid &&
                (!job.existed || guid == job.snapshot.guid))
            {
                job.metadata = saved["meta"].get<std::string>();
                job.snapshot.guid = guid;
                job.completed = keys.size();
                return true;
            }
        }
    }
    auto source = std::make_shared<schema::TerrainAssetDataT>();
    source->origin = input.recipe.origin;
    source->bounds = input.recipe.bounds;
    source->voxel_size = input.recipe.voxel_size;
    source->block_cells = input.recipe.block_cells;
    source->material_layers = input.recipe.material_layers;
    TerrainReadSnapshot samples;
    samples.source = source;
    samples.source_revision = job.spec.source.revision;
    const TerrainRegionKey sample_key = keys.front();
    for (auto key : keys)
    {
        if (job.cancelled->load())
        {
            error = "Terrain authoring cancelled";
            return false;
        }
        input.key = key;
        schema::TerrainBlockDataT block;
        if (!generate_terrain_block(input, generator,
                                    job.project / "cache/terrain-authoring/blocks", block, error))
        {
            return false;
        }
        const auto path = artifact_root / terrain_region_name(key);
        fs::create_directories(path.parent_path(), ec);
        if (ec)
        {
            error = "Cannot create terrain generated block directory";
            return false;
        }
        flatbuffers::FlatBufferBuilder builder;
        schema::FinishTerrainBlockDataBuffer(builder,
                                             schema::TerrainBlockData::Pack(builder, &block));
        const auto bytes = std::span(reinterpret_cast<const std::byte*>(builder.GetBufferPointer()),
                                     builder.GetSize());
        auto pending = path;
        pending += ".pending-" + io::new_guid();
        if (!io::write_bytes(pending, bytes, error))
        {
            return false;
        }
        if (!SDL_RenamePath(PathUtils::to_utf8(pending).c_str(), PathUtils::to_utf8(path).c_str()))
        {
            error = "Cannot publish generated terrain block";
            fs::remove(pending, ec);
            return false;
        }
        auto region = std::make_unique<schema::TerrainRegionEntryT>();
        region->key = key;
        region->uniform = schema::TerrainUniformState::mixed;
        source->regions.push_back(std::move(region));
        if (key.lod() == sample_key.lod() &&
            std::abs(std::int64_t(key.x()) - sample_key.x()) <= 1 &&
            std::abs(std::int64_t(key.y()) - sample_key.y()) <= 1 &&
            std::abs(std::int64_t(key.z()) - sample_key.z()) <= 1)
        {
            samples.blocks[key] = std::make_shared<schema::TerrainBlockDataT>(std::move(block));
        }
        ++job.completed;
    }
    std::sort(source->regions.begin(), source->regions.end(),
              [](const auto& a, const auto& b) { return TerrainRegionLess{}(a->key, b->key); });
    TerrainMeshInput preview;
    preview.samples = std::move(samples);
    preview.key = sample_key;
    preview.cancel_requested = job.cancelled;
    TerrainMeshOutput geometry;
    if (!mesher.build(preview, geometry, error))
    {
        return false;
    }
    const nlohmann::json journal = {
        {"version", 1},          {"generator", std::string(generator.fingerprint())},
        {"path", job.spec.path}, {"guid", job.snapshot.guid},
        {"before", job.before},  {"after", job.after},
        {"meta", job.metadata},  {"sourceHash", job.snapshot.content_hash},
        {"existed", job.existed}};
    if (!atomic_text(job.workspace / "prepared.json", journal.dump(), error))
    {
        return false;
    }
    // A completed marker is written only after every block and preflight succeeded.
    return atomic_text(artifact_root / "complete", job.snapshot.content_hash, error);
}
class TerrainSourceEdit final : public editor::EditorEdit
{
public:
    explicit TerrainSourceEdit(std::shared_ptr<TerrainAuthoringJob> job) : job_(std::move(job))
    {
    }
    ~TerrainSourceEdit() noexcept override
    {
        if (!published_ && job_->snapshot.state == TerrainAuthoringState::ready_to_publish)
        {
            job_->edit_issued = false;
            job_->snapshot.can_publish = true;
            job_->snapshot.can_cancel = true;
        }
    }
    bool affects_scene() const noexcept override
    {
        return false;
    }
    bool apply(std::string& error) override
    {
        return change(false, error);
    }
    bool revert(std::string& error) override
    {
        return change(true, error);
    }

private:
    bool change(bool undo, std::string& error)
    {
        auto& job = *job_;
        io::ProjectLock lock;
        bool locked = false;
        if (!lock.try_acquire(job.project / "cache/.asset-build.lock", locked, error))
        {
            return false;
        }
        if (!locked)
        {
            error = "Resource import is active; retry terrain publication";
            return false;
        }
        fs::path destination;
        if (!terrain_file_path(job.project, "assets", job.spec.path, ".terrain", destination,
                               error))
        {
            return false;
        }
        const bool expect_file = undo || job.existed;
        std::string current;
        std::error_code ec;
        const bool exists = fs::exists(destination, ec);
        if (ec || exists != expect_file ||
            (exists && (!editor::file_detail::read_text(destination, current) ||
                        current != (undo ? job.after : job.before))))
        {
            error = "Terrain source changed outside this edit";
            return false;
        }
        auto meta = destination;
        meta += ".meta";
        std::string current_meta;
        const auto meta_status = fs::symlink_status(meta, ec);
        if (ec == std::errc::no_such_file_or_directory)
        {
            ec.clear();
        }
        if (ec || fs::is_symlink(meta_status))
        {
            error = "Invalid terrain meta path";
            return false;
        }
        if (expect_file && (!editor::file_detail::read_text(meta, current_meta) ||
                            !retain_import_metadata(job, std::move(current_meta), error)))
        {
            return false;
        }
        if (!job.existed && undo)
        {
            const auto retired = job.workspace / "undo-source";
            fs::rename(destination, retired, ec);
            if (ec)
            {
                error = "Cannot retract terrain source";
                return false;
            }
            fs::remove(meta, ec);
            if (ec)
            {
                std::error_code ignored;
                fs::rename(retired, destination, ignored);
                error = "Cannot retract terrain meta";
                return false;
            }
        }
        else
        {
            if (!job.existed && !undo)
            {
                if (fs::exists(meta, ec) || ec)
                {
                    error = "Terrain meta already exists";
                    return false;
                }
                if (!atomic_text(meta, job.metadata, error))
                {
                    return false;
                }
            }
            if (!atomic_text(destination, undo ? job.before : job.after, error))
            {
                if (!job.existed && !undo)
                {
                    std::error_code ignored;
                    fs::remove(meta, ignored);
                }
                return false;
            }
        }
        // Undo changes the source, but this job remains published and cannot be issued twice.
        published_ = true;
        job.snapshot.state = TerrainAuthoringState::published;
        job.snapshot.can_publish = false;
        job.snapshot.can_cancel = false;
        return true;
    }
    std::shared_ptr<TerrainAuthoringJob> job_;
    bool published_ = false;
};
} // namespace
TerrainAuthoringSystem::TerrainAuthoringSystem(editor::EditorEnvironment& environment,
                                               TaskSystem& tasks,
                                               std::unique_ptr<TerrainGenerator> generator,
                                               std::unique_ptr<TerrainMesher> mesher)
    : environment_(environment), tasks_(tasks), generator_(std::move(generator)),
      mesher_(std::move(mesher))
{
}
TerrainAuthoringSystem::~TerrainAuthoringSystem() noexcept
{
    shutdown();
}
bool TerrainAuthoringSystem::on_initialize(const EngineConfig&)
{
    if (!environment_.is_initialized() || !tasks_.is_initialized() || !generator_ || !mesher_ ||
        generator_->fingerprint().empty())
    {
        return false;
    }
    session_ = std::uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
    return session_ != 0;
}
bool TerrainAuthoringSystem::start(const TerrainAuthoringSpec& spec, TerrainAuthoringId& output)
{
    output = {};
    if (!is_initialized() || spec.path.empty() || !spec.source.recipe)
    {
        return false;
    }
    for (const auto& [key, job] : jobs_)
    {
        if (job->spec.path == spec.path &&
            job->spec.expected_source_revision == spec.expected_source_revision &&
            job->spec.source == spec.source &&
            job->snapshot.state != TerrainAuthoringState::cancelled &&
            job->snapshot.state != TerrainAuthoringState::failed)
        {
            output = job->snapshot.id;
            return true;
        }
    }
    if (jobs_.size() >= 32 || sequence_ == UINT64_MAX)
    {
        return false;
    }
    auto job = std::make_shared<TerrainAuthoringJob>();
    job->spec = spec;
    job->snapshot.id = {session_, ++sequence_};
    job->snapshot.path = spec.path;
    // EditorEnvironment installs this project's cache as App's read root before the system starts.
    job->project = App::assets_path().parent_path();
    job->workspace = App::assets_path() / "terrain-authoring" / std::to_string(session_) /
                     std::to_string(sequence_);
    jobs_.emplace(sequence_, job);
    output = job->snapshot.id;
    return true;
}
bool TerrainAuthoringSystem::status(TerrainAuthoringId id, TerrainAuthoringSnapshot& output) const
{
    output = {};
    const auto found = jobs_.find(id.sequence);
    if (id.session != session_ || found == jobs_.end())
    {
        return false;
    }
    const auto& job = *found->second;
    if (job.snapshot.state == TerrainAuthoringState::preparing)
    {
        // Worker fields become visible only after future completion; counters are independent.
        output.id = id;
        output.path = job.spec.path;
        output.state = TerrainAuthoringState::preparing;
        output.completed = job.completed.load();
        output.total = job.total.load();
        output.can_cancel = true;
    }
    else
    {
        output = job.snapshot;
    }
    return true;
}
bool TerrainAuthoringSystem::cancel(TerrainAuthoringId id)
{
    auto found = jobs_.find(id.sequence);
    if (id.session != session_ || found == jobs_.end() || !found->second->snapshot.can_cancel ||
        found->second->edit_issued)
    {
        return false;
    }
    auto& job = *found->second;
    *job.cancelled = true;
    if (!job.future.valid())
    {
        job.snapshot.state = TerrainAuthoringState::cancelled;
        job.snapshot.can_cancel = false;
        job.snapshot.can_publish = false;
    }
    return true;
}
bool TerrainAuthoringSystem::make_edit(TerrainAuthoringId id,
                                       std::unique_ptr<editor::EditorEdit>& output)
{
    output.reset();
    auto found = jobs_.find(id.sequence);
    if (id.session != session_ || found == jobs_.end() ||
        found->second->snapshot.state != TerrainAuthoringState::ready_to_publish ||
        found->second->edit_issued)
    {
        return false;
    }
    found->second->edit_issued = true;
    found->second->snapshot.can_cancel = false;
    found->second->snapshot.can_publish = false;
    output = std::make_unique<TerrainSourceEdit>(found->second);
    return true;
}
bool TerrainAuthoringSystem::release(TerrainAuthoringId id)
{
    auto found = jobs_.find(id.sequence);
    if (id.session != session_ || found == jobs_.end() || found->second->future.valid() ||
        found->second->snapshot.state == TerrainAuthoringState::queued)
    {
        return false;
    }
    jobs_.erase(found);
    return true;
}
bool TerrainAuthoringSystem::on_tick(double)
{
    for (auto& [id, owned] : jobs_)
    {
        auto& job = *owned;
        if (!job.future.valid())
        {
            continue;
        }
        if (job.future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        {
            return true;
        }
        const bool success = job.future.get();
        job.snapshot.completed = job.completed.load();
        job.snapshot.total = job.total.load();
        job.snapshot.state = job.cancelled->load() ? TerrainAuthoringState::cancelled
                             : !success            ? TerrainAuthoringState::failed
                             : job.source_already_published
                                 ? TerrainAuthoringState::published
                                 : TerrainAuthoringState::ready_to_publish;
        job.snapshot.can_publish = job.snapshot.state == TerrainAuthoringState::ready_to_publish;
        job.snapshot.can_cancel = job.snapshot.can_publish;
        if (!success && !job.cancelled->load())
        {
            job.snapshot.error_code = "preparation_failed";
        }
    }
    for (auto& [id, owned] : jobs_)
    {
        if (owned->snapshot.state == TerrainAuthoringState::queued)
        {
            owned->snapshot.state = TerrainAuthoringState::preparing;
            owned->future = tasks_.submit("Terrain authoring", [this, job = owned]()
                                          { return prepare_job(*job, *generator_, *mesher_); });
            if (!owned->future.valid())
            {
                owned->snapshot.state = TerrainAuthoringState::queued;
            }
            break;
        }
    }
    return true;
}
void TerrainAuthoringSystem::on_shutdown() noexcept
{
    for (auto& [id, job] : jobs_)
    {
        *job->cancelled = true;
    }
    for (auto& [id, job] : jobs_)
    {
        if (job->future.valid())
        {
            job->future.wait();
        }
    }
    jobs_.clear();
    session_ = 0;
}
} // namespace engine::plugins::terrain
