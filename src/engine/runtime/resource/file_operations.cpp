#include "runtime/resource/virtual_file_system.hpp"

#include "runtime/utils/path_utils.hpp"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_iostream.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <system_error>

namespace engine
{
namespace
{
namespace fs = std::filesystem;

bool file_fail(std::string& error, std::string_view message, const std::error_code& code = {})
{
    error.assign(message);
    if (code)
    {
        error += ": " + code.message();
    }
    return false;
}

bool valid_path(const fs::path& path, std::string& error)
{
    const auto text = PathUtils::to_utf8(path);
    return (path.is_absolute() && !text.empty() && text.find('\0') == std::string::npos) ||
           file_fail(error, "Expected an absolute native file path");
}

bool path_status(const fs::path& path, fs::file_status& status, std::string& error)
{
    if (!valid_path(path, error))
    {
        return false;
    }
    std::error_code code;
    status = fs::symlink_status(path, code);
    if (code == std::errc::no_such_file_or_directory)
    {
        code.clear();
    }
    return !code || file_fail(error, "Cannot inspect path", code);
}

bool destination_missing(const fs::path& path, std::string& error)
{
    fs::file_status status;
    return path_status(path, status, error) &&
           (!fs::exists(status) || file_fail(error, "Destination already exists"));
}

bool read_entry(const fs::path& path, FileEntry& output, std::string& error)
{
    fs::file_status status;
    if (!path_status(path, status, error) || !fs::exists(status))
    {
        return error.empty() ? file_fail(error, "Path does not exist") : false;
    }
    FileEntry entry;
    entry.path = path;
    if (fs::is_regular_file(status))
    {
        entry.kind = FileKind::file;
        SDL_PathInfo info{};
        if (!SDL_GetPathInfo(PathUtils::to_utf8(path).c_str(), &info))
        {
            return file_fail(error, "Cannot read file size: " + std::string(SDL_GetError()));
        }
        entry.size = info.size;
    }
    else if (fs::is_directory(status))
    {
        entry.kind = FileKind::directory;
    }
    else if (fs::is_symlink(status))
    {
        entry.kind = FileKind::symlink;
    }
    output = std::move(entry);
    return true;
}

fs::path temporary_path(const fs::path& path)
{
    static std::atomic<std::uint64_t> sequence{0};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto temporary = path;
    temporary += ".tmp-" + std::to_string(stamp) + "-" + std::to_string(sequence.fetch_add(1));
    return temporary;
}

struct DirectoryEntries
{
    std::vector<FileEntry> entries;
    std::string error;
};

SDL_EnumerationResult SDLCALL enumerate_entry(void* userdata, const char* directory,
                                              const char* name)
{
    auto& result = *static_cast<DirectoryEntries*>(userdata);
    FileEntry entry;
    if (!read_entry(PathUtils::from_utf8(directory) / PathUtils::from_utf8(name), entry,
                    result.error))
    {
        return SDL_ENUM_FAILURE;
    }
    result.entries.push_back(std::move(entry));
    return SDL_ENUM_CONTINUE;
}
} // namespace

bool VirtualFileSystem::stat(const fs::path& path, FileEntry& output, std::string& error)
{
    output = {};
    error.clear();
    return read_entry(path, output, error);
}

bool VirtualFileSystem::list_directory(const fs::path& path, std::vector<FileEntry>& output,
                                       std::string& error)
{
    output.clear();
    error.clear();
    fs::file_status status;
    if (!path_status(path, status, error))
    {
        return false;
    }
    if (!fs::is_directory(status))
    {
        return file_fail(error, "Expected a directory, not a symbolic link");
    }
    DirectoryEntries result;
    if (!SDL_EnumerateDirectory(PathUtils::to_utf8(path).c_str(), enumerate_entry, &result))
    {
        return file_fail(error, result.error.empty()
                                    ? "Cannot enumerate directory: " + std::string(SDL_GetError())
                                    : result.error);
    }
    std::sort(result.entries.begin(), result.entries.end(),
              [](const FileEntry& left, const FileEntry& right)
              {
                  return left.path.filename().generic_u8string() <
                         right.path.filename().generic_u8string();
              });
    output = std::move(result.entries);
    return true;
}

bool VirtualFileSystem::write_file(const fs::path& path, ByteView bytes, std::string& error)
{
    error.clear();
    fs::file_status status;
    if (!path_status(path, status, error))
    {
        return false;
    }
    if (fs::exists(status) && !fs::is_regular_file(status))
    {
        return file_fail(error, "Write destination must be a regular file");
    }
    const auto temporary = temporary_path(path);
    auto* stream = SDL_IOFromFile(PathUtils::to_utf8(temporary).c_str(), "wbx");
    if (!stream)
    {
        return file_fail(error, "Cannot create temporary file: " + std::string(SDL_GetError()));
    }
    std::size_t written = 0;
    while (written < bytes.size())
    {
        const auto count = SDL_WriteIO(stream, bytes.data() + written, bytes.size() - written);
        if (count == 0)
        {
            break;
        }
        written += count;
    }
    const bool closed = SDL_CloseIO(stream);
    const bool published =
        written == bytes.size() && closed &&
        SDL_RenamePath(PathUtils::to_utf8(temporary).c_str(), PathUtils::to_utf8(path).c_str());
    if (!published)
    {
        file_fail(error, "Cannot write or publish file: " + std::string(SDL_GetError()));
        SDL_RemovePath(PathUtils::to_utf8(temporary).c_str());
        return false;
    }
    return true;
}

bool VirtualFileSystem::create_directory(const fs::path& path, std::string& error)
{
    error.clear();
    fs::file_status status;
    if (!path_status(path, status, error))
    {
        return false;
    }
    if (fs::is_directory(status))
    {
        return true;
    }
    if (fs::exists(status))
    {
        return file_fail(error, "Path already exists and is not a directory");
    }
    SDL_PathInfo parent{};
    if (!SDL_GetPathInfo(PathUtils::to_utf8(path.parent_path()).c_str(), &parent) ||
        parent.type != SDL_PATHTYPE_DIRECTORY)
    {
        return file_fail(error, "Parent directory must exist");
    }
    return SDL_CreateDirectory(PathUtils::to_utf8(path).c_str()) ||
           file_fail(error, "Cannot create directory: " + std::string(SDL_GetError()));
}

bool VirtualFileSystem::copy_file(const fs::path& source, const fs::path& destination,
                                  std::string& error)
{
    error.clear();
    fs::file_status status;
    if (!path_status(source, status, error) || !destination_missing(destination, error))
    {
        return false;
    }
    if (!fs::is_regular_file(status))
    {
        return file_fail(error, "Copy source must be a regular file");
    }
    const auto temporary = temporary_path(destination);
    const auto temporary_text = PathUtils::to_utf8(temporary);
    auto* reservation = SDL_IOFromFile(temporary_text.c_str(), "wbx");
    if (!reservation)
    {
        return file_fail(error, "Cannot create copy destination: " + std::string(SDL_GetError()));
    }
    const bool copied = SDL_CloseIO(reservation) &&
                        SDL_CopyFile(PathUtils::to_utf8(source).c_str(), temporary_text.c_str());
    if (!copied || !destination_missing(destination, error) ||
        !SDL_RenamePath(temporary_text.c_str(), PathUtils::to_utf8(destination).c_str()))
    {
        if (error.empty())
        {
            file_fail(error, "Cannot copy or publish file: " + std::string(SDL_GetError()));
        }
        SDL_RemovePath(temporary_text.c_str());
        return false;
    }
    return true;
}

bool VirtualFileSystem::move(const fs::path& source, const fs::path& destination,
                             std::string& error)
{
    error.clear();
    fs::file_status status;
    if (!path_status(source, status, error) || !destination_missing(destination, error))
    {
        return false;
    }
    if (!fs::is_regular_file(status) && !fs::is_directory(status) && !fs::is_symlink(status))
    {
        return file_fail(error, "Move source must be a file, directory or symbolic link");
    }
    return SDL_RenamePath(PathUtils::to_utf8(source).c_str(),
                          PathUtils::to_utf8(destination).c_str()) ||
           file_fail(error, "Cannot move path: " + std::string(SDL_GetError()));
}

bool VirtualFileSystem::remove(const fs::path& path, std::string& error)
{
    error.clear();
    fs::file_status status;
    if (!path_status(path, status, error))
    {
        return false;
    }
    if (!fs::exists(status))
    {
        return true;
    }
    if (!fs::is_regular_file(status) && !fs::is_directory(status) && !fs::is_symlink(status))
    {
        return file_fail(error, "Cannot remove a special file");
    }
    return SDL_RemovePath(PathUtils::to_utf8(path).c_str()) ||
           file_fail(error,
                     "Cannot remove file or empty directory: " + std::string(SDL_GetError()));
}
} // namespace engine
