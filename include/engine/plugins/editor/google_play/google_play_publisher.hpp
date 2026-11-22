#pragma once

#include <filesystem>
#include <future>
#include <string>

#include <nlohmann/json.hpp>

namespace engine
{
class TaskSystem;
namespace editor
{
class EditorCommands;
}
} // namespace engine

namespace engine::plugins::google_play
{
class GooglePlayPublisher final
{
public:
    GooglePlayPublisher(TaskSystem& tasks, std::filesystem::path project,
                        std::filesystem::path sdk);
    ~GooglePlayPublisher() noexcept;
    [[nodiscard]] bool register_commands(editor::EditorCommands& commands);

private:
    void poll();
    TaskSystem& tasks_;
    std::filesystem::path project_;
    std::filesystem::path sdk_;
    std::future<bool> pending_;
    nlohmann::json status_ = {{"state", "idle"}};
    nlohmann::json receipt_;
};
} // namespace engine::plugins::google_play
