#include "plugins/runtime/terrain/terrain_state_store.hpp"

#include "plugins/runtime/terrain/terrain_samples.hpp"
#include "runtime/core/guid_map.hpp"
#include "runtime/utils/hash_utils.hpp"
#include "runtime/utils/path_utils.hpp"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_timer.h>
#include <flatbuffers/flatbuffers.h>
#include <flatbuffers/verifier.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <set>
#include <system_error>

namespace engine::plugins::terrain
{
namespace
{
namespace fs = std::filesystem;
bool owned_path(const fs::path& root, const fs::path& path, std::string& error)
{
    const auto relative = path.lexically_relative(root);
    if (root.empty() || relative.empty() || relative.is_absolute())
    {
        error = "Invalid terrain storage path";
        return false;
    }
    auto current = root;
    const auto check = [&]()
    {
        std::error_code ec;
        const auto status = fs::symlink_status(current, ec);
        if ((ec && ec != std::errc::no_such_file_or_directory) || fs::is_symlink(status))
        {
            error = "Terrain storage paths must not contain symbolic links";
            return false;
        }
        return true;
    };
    if (!check())
    {
        return false;
    }
    for (const auto& part : relative)
    {
        if (part == "..")
        {
            error = "Terrain storage path escapes its root";
            return false;
        }
        current /= part;
        if (!check())
        {
            return false;
        }
    }
    return true;
}
bool safe_token(std::string_view value)
{
    return !value.empty() && value.size() <= 96 &&
           std::all_of(value.begin(), value.end(),
                       [](char c)
                       {
                           return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                  (c >= '0' && c <= '9') || c == '-' || c == '_';
                       });
}
bool content_key(std::string_view value)
{
    return value.size() == 16 &&
           std::all_of(value.begin(), value.end(),
                       [](char c) { return (c >= 'a' && c <= 'f') || (c >= '0' && c <= '9'); });
}
ByteView view(const flatbuffers::FlatBufferBuilder& builder)
{
    return std::as_bytes(std::span(builder.GetBufferPointer(), builder.GetSize()));
}
bool read_bounded(const fs::path& path, std::uint64_t limit, ByteBuffer& output, std::string& error)
{
    output.clear();
    FileEntry entry;
    if (!VirtualFileSystem::stat(path, entry, error) || entry.kind != FileKind::file ||
        entry.size == 0 || entry.size > limit)
    {
        error = "Terrain checkpoint file is missing, unsafe or exceeds its size limit";
        return false;
    }
    return VirtualFileSystem::read_file(path, output, &error) && output.size() == entry.size;
}
bool validate(const schema::TerrainCheckpointDataT& data, std::string& error)
{
    if (data.schema_version != 2 || !GuidMap::valid_guid(data.base_guid) ||
        !content_key(data.base_hash) || !safe_token(data.checkpoint_id) || data.revision == 0 ||
        data.next_debris_id == 0 || data.regions.size() > 1024U * 1024U ||
        data.debris.size() > 4096 || data.retired_ids.size() > 1024U * 1024U)
    {
        error = "Invalid terrain checkpoint identity, version or capacity";
        return false;
    }
    const auto regions_valid = [&](const auto& regions)
    {
        std::set<TerrainRegionKey, TerrainRegionLess> keys;
        for (const auto& region : regions)
        {
            if (!region || region->key.lod() > 15 || !content_key(region->content_hash) ||
                region->payload_key != region->content_hash || !keys.insert(region->key).second)
            {
                return false;
            }
        }
        return true;
    };
    if (!regions_valid(data.regions))
    {
        error = "Invalid terrain checkpoint region references";
        return false;
    }
    std::set<std::uint64_t> identities;
    for (const auto& debris : data.debris)
    {
        if (!debris || debris->stable_id == 0 || debris->stable_id >= data.next_debris_id ||
            !identities.insert(debris->stable_id).second || debris->blocks.empty() ||
            !regions_valid(debris->blocks) || !terrain_detail::valid_bounds(debris->bounds) ||
            !terrain_detail::finite(debris->position) ||
            !terrain_detail::finite(debris->linear_velocity) ||
            !terrain_detail::finite(debris->angular_velocity) || !std::isfinite(debris->density) ||
            debris->density <= 0 || !std::isfinite(debris->simulation_age) ||
            debris->simulation_age < 0)
        {
            error = "Invalid terrain checkpoint debris identity, payload or motion";
            return false;
        }
        const auto& rotation = debris->rotation;
        const double norm = rotation.x() * rotation.x() + rotation.y() * rotation.y() +
                            rotation.z() * rotation.z() + rotation.w() * rotation.w();
        if (!std::isfinite(norm) || std::abs(norm - 1) > 1e-5)
        {
            error = "Terrain checkpoint debris rotation must be a unit quaternion";
            return false;
        }
    }
    for (auto id : data.retired_ids)
    {
        if (id == 0 || id >= data.next_debris_id || !identities.insert(id).second)
        {
            error = "Invalid or duplicate retired terrain debris identity";
            return false;
        }
    }
    return true;
}
} // namespace
TerrainStateStore::TerrainStateStore(std::filesystem::path root) : root_(std::move(root))
{
}
TerrainStateStore::~TerrainStateStore() noexcept
{
    cancel();
}

bool TerrainStateStore::stage(const schema::TerrainCheckpointDataT& snapshot,
                              std::span<const schema::TerrainBlockDataT> blocks,
                              std::string_view slot, std::string& error)
{
    error.clear();
    if (!staged_.empty() || root_.empty() || !safe_token(slot) || !validate(snapshot, error))
    {
        if (error.empty())
        {
            error = "Terrain checkpoint store is busy or its slot is invalid";
        }
        return false;
    }
    const auto directory = root_ / std::string(slot);
    if (!owned_path(root_, directory / "blocks", error) ||
        !owned_path(root_, directory / "checkpoints", error) ||
        !owned_path(root_, directory / "current", error))
    {
        return false;
    }
    if (!VirtualFileSystem::create_directory(root_, error) ||
        !VirtualFileSystem::create_directory(directory, error) ||
        !VirtualFileSystem::create_directory(directory / "blocks", error) ||
        !VirtualFileSystem::create_directory(directory / "checkpoints", error))
    {
        return false;
    }
    const auto manifest = directory / "current";
    std::error_code status_error;
    const auto status = fs::symlink_status(manifest, status_error);
    std::string before;
    if (!status_error && fs::exists(status))
    {
        ByteBuffer bytes;
        if (!read_bounded(manifest, 64U * 1024U * 1024U, bytes, error))
        {
            return false;
        }
        before = hash::hex(hash::fnv1a(bytes));
    }
    else if (status_error && status_error != std::errc::no_such_file_or_directory)
    {
        error = "Cannot inspect the current terrain checkpoint";
        return false;
    }
    for (const auto& block : blocks)
    {
        flatbuffers::FlatBufferBuilder builder;
        schema::FinishTerrainBlockDataBuffer(builder,
                                             schema::TerrainBlockData::Pack(builder, &block));
        const auto bytes = view(builder);
        const auto key = hash::hex(hash::fnv1a(bytes));
        const auto path = directory / "blocks" / key;
        if (!owned_path(root_, path, error))
        {
            return false;
        }
        const auto existing = fs::symlink_status(path, status_error);
        if (!status_error && fs::exists(existing))
        {
            ByteBuffer previous;
            if (!read_bounded(path, 4U * 1024U * 1024U, previous, error) ||
                previous.size() != bytes.size() ||
                !std::equal(previous.begin(), previous.end(), bytes.begin()))
            {
                error = "Terrain checkpoint payload collision or corruption";
                return false;
            }
        }
        else if (!VirtualFileSystem::write_file(path, bytes, error))
        {
            return false;
        }
    }
    const auto validate_references = [&](const auto& references)
    {
        for (const auto& region : references)
        {
            schema::TerrainBlockDataT payload;
            if (!read_block(slot, region->payload_key, payload, error) ||
                payload.key != region->key)
            {
                return false;
            }
        }
        return true;
    };
    if (!validate_references(snapshot.regions))
    {
        return false;
    }
    for (const auto& debris : snapshot.debris)
    {
        if (!validate_references(debris->blocks))
        {
            return false;
        }
    }
    flatbuffers::FlatBufferBuilder builder;
    schema::FinishTerrainCheckpointDataBuffer(
        builder, schema::TerrainCheckpointData::Pack(builder, &snapshot));
    const auto bytes = view(builder);
    const auto archive = directory / "checkpoints" / snapshot.checkpoint_id;
    if (!owned_path(root_, archive, error))
    {
        return false;
    }
    const auto archive_status = fs::symlink_status(archive, status_error);
    if (!status_error && fs::exists(archive_status))
    {
        ByteBuffer previous;
        if (!read_bounded(archive, 64U * 1024U * 1024U, previous, error) ||
            previous.size() != bytes.size() ||
            !std::equal(previous.begin(), previous.end(), bytes.begin()))
        {
            error = "Terrain checkpoint ID already identifies different content";
            return false;
        }
    }
    else if (!VirtualFileSystem::write_file(archive, bytes, error))
    {
        return false;
    }
    static std::atomic<std::uint64_t> sequence{0};
    const auto pending = directory / ("pending-" + std::to_string(SDL_GetTicksNS()) + "-" +
                                      std::to_string(sequence.fetch_add(1)));
    if (!VirtualFileSystem::write_file(pending, bytes, error))
    {
        return false;
    }
    staged_ = pending;
    manifest_ = manifest;
    previous_hash_ = std::move(before);
    return true;
}

bool TerrainStateStore::publish(std::string& error)
{
    error.clear();
    if (staged_.empty())
    {
        error = "No staged terrain checkpoint";
        return false;
    }
    if (!owned_path(root_, staged_, error) || !owned_path(root_, manifest_, error))
    {
        return false;
    }
    std::error_code status_error;
    const auto status = fs::symlink_status(manifest_, status_error);
    std::string current;
    if (!status_error && fs::exists(status))
    {
        ByteBuffer bytes;
        if (!read_bounded(manifest_, 64U * 1024U * 1024U, bytes, error))
        {
            return false;
        }
        current = hash::hex(hash::fnv1a(bytes));
    }
    else if (status_error && status_error != std::errc::no_such_file_or_directory)
    {
        error = "Cannot inspect terrain checkpoint publication target";
        return false;
    }
    if (current != previous_hash_ ||
        !SDL_RenamePath(PathUtils::to_utf8(staged_).c_str(), PathUtils::to_utf8(manifest_).c_str()))
    {
        error = "Terrain checkpoint publication conflict or file replacement failure";
        return false;
    }
    staged_.clear();
    manifest_.clear();
    previous_hash_.clear();
    return true;
}

bool TerrainStateStore::read(std::string_view slot, schema::TerrainCheckpointDataT& output,
                             std::string& error) const
{
    output = {};
    error.clear();
    ByteBuffer bytes;
    if (!safe_token(slot) || !owned_path(root_, root_ / std::string(slot) / "current", error) ||
        !read_bounded(root_ / std::string(slot) / "current", 64U * 1024U * 1024U, bytes, error))
    {
        if (error.empty())
        {
            error = "Invalid terrain checkpoint slot";
        }
        return false;
    }
    const auto* data = reinterpret_cast<const std::uint8_t*>(bytes.data());
    flatbuffers::Verifier verifier(data, bytes.size());
    if (!schema::VerifyTerrainCheckpointDataBuffer(verifier))
    {
        error = "Invalid terrain checkpoint format";
        return false;
    }
    auto candidate = schema::UnPackTerrainCheckpointData(data);
    if (!validate(*candidate, error))
    {
        return false;
    }
    output = std::move(*candidate);
    return true;
}

bool TerrainStateStore::read_block(std::string_view slot, std::string_view key,
                                   schema::TerrainBlockDataT& output, std::string& error) const
{
    output = {};
    error.clear();
    ByteBuffer bytes;
    if (!safe_token(slot) || !content_key(key) ||
        !owned_path(root_, root_ / std::string(slot) / "blocks" / std::string(key), error) ||
        !read_bounded(root_ / std::string(slot) / "blocks" / std::string(key), 4U * 1024U * 1024U,
                      bytes, error) ||
        hash::hex(hash::fnv1a(bytes)) != key)
    {
        if (error.empty())
        {
            error = "Terrain checkpoint block path or hash is invalid";
        }
        return false;
    }
    const auto* data = reinterpret_cast<const std::uint8_t*>(bytes.data());
    flatbuffers::Verifier verifier(data, bytes.size());
    if (!schema::VerifyTerrainBlockDataBuffer(verifier))
    {
        error = "Invalid terrain checkpoint block format";
        return false;
    }
    auto candidate = schema::UnPackTerrainBlockData(data);
    const auto count = candidate->density.size();
    if (candidate->schema_version != 2 || candidate->key.lod() > 15 ||
        (candidate->uniform ? count != 1 : count != 512 && count != 4096 && count != 32768) ||
        candidate->layers.size() != count ||
        !std::all_of(candidate->density.begin(), candidate->density.end(),
                     [](float v) { return std::isfinite(v); }) ||
        !std::all_of(candidate->layers.begin(), candidate->layers.end(),
                     [](const auto& layer) { return terrain_detail::layer_valid(layer, 256); }))
    {
        error = "Invalid terrain checkpoint block samples";
        return false;
    }
    output = std::move(*candidate);
    return true;
}

void TerrainStateStore::cancel() noexcept
{
    if (!staged_.empty())
    {
        std::string ignored;
        if (owned_path(root_, staged_, ignored))
        {
            (void)VirtualFileSystem::remove(staged_, ignored);
        }
    }
    staged_.clear();
    manifest_.clear();
    previous_hash_.clear();
}
} // namespace engine::plugins::terrain
