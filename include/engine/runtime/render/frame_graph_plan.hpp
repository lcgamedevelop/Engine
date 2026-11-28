#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace engine
{

using FramePassId = std::uint16_t;
using FrameResourceId = std::uint16_t;
inline constexpr auto frame_invalid_id = std::numeric_limits<std::uint16_t>::max();
inline constexpr std::size_t frame_pass_capacity = 256;
inline constexpr std::size_t frame_resource_capacity = 512;

// Resource IDs name single-assignment versions. Order predecessors constrain surviving work.
struct FramePassDeclaration
{
    FramePassId id = frame_invalid_id;
    std::span<const FrameResourceId> reads{};
    std::span<const FrameResourceId> writes{};
    std::span<const FramePassId> after{};
    bool side_effect = false;
};

struct FramePlanError
{
    const char* reason = nullptr;
    FramePassId pass = frame_invalid_id;
    FrameResourceId resource = frame_invalid_id;
    FramePassId predecessor = frame_invalid_id;
    // A closed chain of active passes, valid until the next compile or plan destruction.
    std::span<const FramePassId> cycle{};
};

class FrameGraphPlan final
{
public:
    FrameGraphPlan() = default;
    FrameGraphPlan(const FrameGraphPlan&) = delete;
    FrameGraphPlan& operator=(const FrameGraphPlan&) = delete;

    [[nodiscard]] bool compile(std::span<const FramePassDeclaration> passes,
                               std::span<const FrameResourceId> imports,
                               std::span<const FrameResourceId> roots,
                               FramePlanError& error) noexcept;
    [[nodiscard]] std::span<const FramePassId> passes() const noexcept
    {
        return {scheduled_.data(), count_};
    }

    // The owner has matched all structure against its cache key, and starts another frame.
    [[nodiscard]] bool begin_cached_frame() noexcept;
    template <typename Submit> [[nodiscard]] bool perform(Submit&& submit, FramePlanError& error)
    {
        error = {};
        if (state_ != State::ready)
        {
            error.reason = "Frame plan requires a new compiled or cached frame";
            return false;
        }
        state_ = State::executing;
        for (const auto pass : passes())
        {
            if (!submit(pass))
            {
                state_ = State::consumed;
                error = {"Frame pass submission failed", pass};
                return false;
            }
        }
        state_ = State::consumed;
        return true;
    }

private:
    enum class State : std::uint8_t
    {
        invalid,
        ready,
        executing,
        consumed
    };
    std::array<FramePassId, frame_pass_capacity> scheduled_{};
    std::array<FramePassId, frame_pass_capacity + 1> cycle_{};
    std::uint16_t count_ = 0;
    State state_ = State::invalid;
};

} // namespace engine
