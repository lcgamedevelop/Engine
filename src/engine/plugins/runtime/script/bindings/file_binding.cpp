#include "plugins/runtime/script/bindings/file_binding.hpp"
#include "runtime/performance/profiling.hpp"

#include "plugins/runtime/script/bindings/binding_context.hpp"
#include "plugins/runtime/script/bindings/data_binding_helpers.hpp"
#include "runtime/utils/path_utils.hpp"

namespace engine::plugins::script::bindings
{
namespace
{
constexpr std::string_view javascript = R"js(
export class File {
    constructor() { throw new TypeError('File is static'); }
    static ReadText(...args) { return native.FileReadText(...args); }
    static ReadBytes(...args) { return native.FileReadBytes(...args); }
    static WriteText(...args) { return native.FileWriteText(...args); }
    static WriteBytes(...args) { return native.FileWriteBytes(...args); }
    static Stat(...args) { return native.FileStat(...args); }
    static List(...args) { return native.FileList(...args); }
    static CreateDirectory(...args) { return native.FileCreateDirectory(...args); }
    static Copy(...args) { return native.FileCopy(...args); }
    static Move(...args) { return native.FileMove(...args); }
    static Remove(...args) { return native.FileRemove(...args); }
    static get lastError() { return native.FileLastError(); }
}
Object.freeze(File.prototype);
Object.freeze(File);
)js";

BindingContext* begin(const Arguments& arguments, int count)
{
    ENGINE_PROFILE_FUNCTION();
    auto* context = BindingContext::current(arguments);
    if (context)
    {
        context->set_file_error({});
        if (arguments.count() != count)
        {
            context->set_file_error("Invalid file operation argument count");
            return nullptr;
        }
    }
    return context;
}

bool path_argument(BindingContext& context, const Arguments& arguments, int index,
                   std::filesystem::path& output)
{
    ENGINE_PROFILE_FUNCTION();
    std::string text;
    if (!arguments.text(index, text) || text.empty() || text.find('\0') != std::string::npos)
    {
        context.set_file_error("File path must be a nonempty UTF-8 string without NUL");
        return false;
    }
    output = PathUtils::from_utf8(text);
    if (!output.is_absolute() && !text.starts_with("apk:/assets/"))
    {
        output.clear();
        context.set_file_error("File path must be absolute");
        return false;
    }
    return true;
}

void read(const Arguments& arguments)
{
    ENGINE_PROFILE_FUNCTION();
    arguments.set_return({});
    auto* context = begin(arguments, 1);
    std::filesystem::path path;
    if (!context || !path_argument(*context, arguments, 0, path))
    {
        return;
    }
    ByteBuffer bytes;
    std::string error;
    if (!VirtualFileSystem::read_file(path, bytes, &error))
    {
        context->set_file_error(error);
        return;
    }
    if (arguments.data() == 0)
    {
        std::string text;
        if (!binding_detail::decode_text(bytes, text))
        {
            context->set_file_error("File text must be valid UTF-8 and at most 4 MiB");
            return;
        }
        arguments.set_return(std::move(text));
    }
    else
    {
        arguments.set_return(std::move(bytes));
    }
}

void write(const Arguments& arguments)
{
    ENGINE_PROFILE_FUNCTION();
    arguments.set_return(false);
    auto* context = begin(arguments, 2);
    std::filesystem::path path;
    if (!context || !path_argument(*context, arguments, 0, path))
    {
        return;
    }
    ByteBuffer bytes;
    std::string text;
    ByteView view;
    if (arguments.data() == 0)
    {
        if (!arguments.text(1, text))
        {
            context->set_file_error("File text must be valid UTF-8 and at most 4 MiB");
            return;
        }
        view = {reinterpret_cast<const std::byte*>(text.data()), text.size()};
    }
    else
    {
        if (!arguments.bytes(1, bytes))
        {
            context->set_file_error("File bytes must be an unshared Uint8Array of at most 64 MiB");
            return;
        }
        view = bytes;
    }
    std::string error;
    const bool success = VirtualFileSystem::write_file(path, view, error);
    context->set_file_error(error);
    arguments.set_return(success);
}

vm::NativeValue entry_value(const FileEntry& entry)
{
    ENGINE_PROFILE_FUNCTION();
    constexpr const char* names[] = {"file", "directory", "symlink", "other"};
    return vm::NativeValue::Object{{"path", PathUtils::to_utf8(entry.path)},
                                   {"kind", names[static_cast<unsigned>(entry.kind)]},
                                   {"size", entry.size}};
}

void inspect(const Arguments& arguments)
{
    ENGINE_PROFILE_FUNCTION();
    arguments.set_return({});
    auto* context = begin(arguments, 1);
    std::filesystem::path path;
    if (!context || !path_argument(*context, arguments, 0, path))
    {
        return;
    }
    std::string error;
    if (arguments.data() == 0)
    {
        FileEntry entry;
        if (VirtualFileSystem::stat(path, entry, error))
        {
            arguments.set_return(entry_value(entry));
        }
    }
    else
    {
        std::vector<FileEntry> entries;
        if (VirtualFileSystem::list_directory(path, entries, error))
        {
            vm::NativeValue::Array result;
            result.reserve(entries.size());
            for (const auto& entry : entries)
            {
                result.push_back(entry_value(entry));
            }
            arguments.set_return(std::move(result));
        }
    }
    context->set_file_error(error);
}

void change(const Arguments& arguments)
{
    ENGINE_PROFILE_FUNCTION();
    arguments.set_return(false);
    const auto operation = arguments.data();
    auto* context = begin(arguments, operation < 2 ? 1 : 2);
    std::filesystem::path source, destination;
    if (!context || !path_argument(*context, arguments, 0, source) ||
        (operation >= 2 && !path_argument(*context, arguments, 1, destination)))
    {
        return;
    }
    std::string error;
    bool success = false;
    switch (operation)
    {
    case 0:
        success = VirtualFileSystem::create_directory(source, error);
        break;
    case 1:
        success = VirtualFileSystem::remove(source, error);
        break;
    case 2:
        success = VirtualFileSystem::copy_file(source, destination, error);
        break;
    case 3:
        success = VirtualFileSystem::move(source, destination, error);
        break;
    default:
        error = "Unknown file operation";
        break;
    }
    context->set_file_error(error);
    arguments.set_return(success);
}

void last_error(const Arguments& arguments)
{
    ENGINE_PROFILE_FUNCTION();
    const auto* context = BindingContext::current(arguments);
    arguments.set_return(context ? vm::NativeValue(context->file_error()) : vm::NativeValue{});
}
} // namespace

std::string_view FileBinding::name() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return "File";
}
std::string_view FileBinding::source() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return javascript;
}
bool FileBinding::register_functions(vm::ScriptVm& vm) const
{
    ENGINE_PROFILE_FUNCTION();
    return vm.function("FileReadText", read, 0) && vm.function("FileReadBytes", read, 1) &&
           vm.function("FileWriteText", write, 0) && vm.function("FileWriteBytes", write, 1) &&
           vm.function("FileStat", inspect, 0) && vm.function("FileList", inspect, 1) &&
           vm.function("FileCreateDirectory", change, 0) && vm.function("FileRemove", change, 1) &&
           vm.function("FileCopy", change, 2) && vm.function("FileMove", change, 3) &&
           vm.function("FileLastError", last_error);
}
} // namespace engine::plugins::script::bindings
