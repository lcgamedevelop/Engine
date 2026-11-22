#include "plugins/editor/google_play/google_play_publisher.hpp"

#include "editor/asset_pipeline/import_tool.hpp"
#include "editor/commands/editor_commands.hpp"
#include "runtime/task/task_system.hpp"
#include "runtime/utils/path_utils.hpp"

#include <chrono>
#include <cstdlib>
#include <utility>

namespace engine::plugins::google_play
{
namespace fs = std::filesystem;
using Json = nlohmann::json;

GooglePlayPublisher::GooglePlayPublisher(TaskSystem& tasks, fs::path project, fs::path sdk)
    : tasks_(tasks), project_(std::move(project)), sdk_(std::move(sdk))
{
    if (const char* configured = std::getenv("ENGINE_SDK_ROOT"); configured && *configured)
    {
        sdk_ = PathUtils::from_utf8(configured);
    }
    else
    {
        auto candidate = std::move(sdk_);
        sdk_.clear();
        std::error_code error;
        while (!candidate.empty())
        {
            if (fs::is_regular_file(candidate / "platforms/android/editor/google_play.py", error))
            {
                sdk_ = candidate;
                break;
            }
            if (candidate == candidate.root_path())
            {
                break;
            }
            candidate = candidate.parent_path();
        }
    }
}

GooglePlayPublisher::~GooglePlayPublisher() noexcept
{
    if (pending_.valid())
    {
        pending_.wait();
    }
}

void GooglePlayPublisher::poll()
{
    if (pending_.valid() && pending_.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    {
        status_["state"] = pending_.get() ? "completed" : "failed";
        status_["result"] = std::move(receipt_);
    }
}

bool GooglePlayPublisher::register_commands(editor::EditorCommands& commands)
{
    return commands.register_command(
               "start_google_play_release", false,
               [this](const Json& args, Json& result, auto&, std::string& error)
               {
                   poll();
                   if (pending_.valid())
                   {
                       error = "A Google Play operation is already active";
                       return false;
                   }
                   const auto mode = args.find("action");
                   if (mode == args.end() || !mode->is_string() ||
                       (*mode != "inspect" && *mode != "build" && *mode != "publish"))
                   {
                       error = "Select inspect, build or publish";
                       return false;
                   }
                   const auto format = args.find("format");
                   if (format != args.end() &&
                       (*mode != "build" || (*format != "apk" && *format != "aab")))
                   {
                       error = "Select apk or aab for a local build";
                       return false;
                   }
                   const auto worker = sdk_ / "platforms/android/editor/google_play.py";
                   std::error_code code;
                   if (!sdk_.is_absolute() || !fs::is_regular_file(worker, code))
                   {
                       error =
                           "Set ENGINE_SDK_ROOT to the Engine SDK containing the Android publisher";
                       return false;
                   }
                   std::vector<std::string> argv;
                   if (const char* python = std::getenv("ENGINE_GOOGLE_PLAY_PYTHON");
                       python && *python)
                   {
                       argv.emplace_back(python);
                   }
                   else
                   {
                       argv = {"uv", "run", "--quiet", "--no-project", "--no-config", "--script"};
                   }
                   argv.insert(argv.end(), {PathUtils::to_utf8(worker), mode->get<std::string>(),
                                            "--project", PathUtils::to_utf8(project_)});
                   if (format != args.end())
                   {
                       argv.insert(argv.end(), {"--format", format->get<std::string>()});
                   }
                   pending_ = tasks_.submit(
                       "Google Play release",
                       [this, argv = std::move(argv)]
                       {
                           std::string output, diagnostic;
                           const auto done = editor::asset_pipeline::run_import_tool(
                               argv, output, diagnostic, {}, 65536);
                           const auto last = output.find_last_not_of("\r\n");
                           const auto line =
                               last == output.npos ? output.npos : output.rfind('\n', last);
                           receipt_ = Json::parse(output.substr(line == output.npos ? 0 : line + 1),
                                                  nullptr, false);
                           if (!receipt_.is_object())
                           {
                               receipt_ = {{"error", (!output.empty() || diagnostic.empty())
                                                         ? "Publisher returned an invalid receipt"
                                                         : diagnostic}};
                               return false;
                           }
                           return done == editor::asset_pipeline::ImportToolStatus::success &&
                                  !receipt_.contains("error");
                       });
                   if (!pending_.valid())
                   {
                       error = "Editor task domain rejected the release operation";
                       return false;
                   }
                   status_ = {{"state", "running"}, {"action", *mode}};
                   result = status_;
                   return true;
               }) &&
           commands.register_command("get_google_play_status", false,
                                     [this](const Json&, Json& result, auto&, std::string&)
                                     {
                                         poll();
                                         result = status_;
                                         return true;
                                     });
}
} // namespace engine::plugins::google_play
