#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace engine::render_detail
{

// M1a covers full-surface raster writes. Each slot denotes one version, with a single
// writer or an imported, defined image. GPU handles are borrowed only for this frame.
using GraphSlot = std::uint8_t;
using GraphPass = std::uint8_t;
inline constexpr std::uint8_t graph_invalid_id = std::numeric_limits<std::uint8_t>::max();
inline constexpr std::size_t graph_slot_count = 16;
inline constexpr std::size_t graph_pass_count = 8;
inline constexpr std::uint16_t graph_invalid_texture = std::numeric_limits<std::uint16_t>::max();

struct FrameKey
{
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint16_t max_views = 0;
    std::uint8_t samples = 1;
    bool operator==(const FrameKey&) const = default;
};

struct FrameBindings
{
    // All identities are bgfx texture indices, including the color attachment of the output.
    std::array<std::uint16_t, graph_slot_count> textures = []
    {
        std::array<std::uint16_t, graph_slot_count> result;
        result.fill(graph_invalid_texture);
        return result;
    }();
    bool operator==(const FrameBindings&) const = default;
};

struct GraphPassDesc
{
    GraphPass pass = graph_invalid_id;
    std::uint16_t view = 0;
    GraphSlot output = graph_invalid_id;
    std::array<GraphSlot, 2> reads{graph_invalid_id, graph_invalid_id};
    bool operator==(const GraphPassDesc&) const = default;
};

struct GraphDeclaration
{
    std::array<GraphPassDesc, graph_pass_count> passes{};
    std::array<bool, graph_slot_count> imports{};
    std::uint8_t pass_count = 0;
    std::uint8_t slot_count = 0;
    // The surrounding batch grants this slice an inclusive range in increasing View order.
    std::uint16_t first_view = 0;
    std::uint16_t last_view = 0;
    GraphSlot root = graph_invalid_id;
    bool operator==(const GraphDeclaration&) const = default;
};

struct GraphError
{
    const char* reason = nullptr;
    GraphPass pass = graph_invalid_id;
    GraphSlot slot = graph_invalid_id;
};

struct GraphInterval
{
    static constexpr std::uint8_t unused = std::numeric_limits<std::uint8_t>::max();
    std::uint8_t first = unused;
    std::uint8_t last = unused;
};

struct FramePlan
{
    std::array<GraphInterval, graph_slot_count> intervals{};
    std::array<GraphPassDesc, graph_pass_count> scheduled{};
    std::uint8_t count = 0;
    [[nodiscard]] std::span<const GraphPassDesc> passes() const noexcept
    {
        return {scheduled.data(), count};
    }
};

class RenderGraph final
{
public:
    RenderGraph() = default;
    RenderGraph(const RenderGraph&) = delete;
    RenderGraph& operator=(const RenderGraph&) = delete;

    [[nodiscard]] bool begin(const FrameKey& key) noexcept;
    [[nodiscard]] bool compile(const GraphDeclaration& declaration, const FrameBindings& bindings,
                               GraphError& error) noexcept;

    template <typename Submit>
    [[nodiscard]] bool execute(const FrameBindings& bindings, Submit&& submit, GraphError& error)
    {
        if (!consume(bindings, error))
        {
            return false;
        }
        for (const auto& segment : plan_.passes())
        {
            if (!submit(segment))
            {
                error = {"Render graph segment submission failed", segment.pass, segment.output};
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool cache_hit() const noexcept
    {
        return cache_hit_;
    }
    [[nodiscard]] const FramePlan& plan() const noexcept
    {
        return plan_;
    }
    void reset() noexcept;

private:
    [[nodiscard]] bool consume(const FrameBindings& bindings, GraphError& error) noexcept;

    enum class Phase : std::uint8_t
    {
        idle,
        declaring,
        compiled
    };
    Phase phase_ = Phase::idle;
    FrameKey key_{};
    FrameBindings bindings_{};
    FrameKey cached_key_{};
    GraphDeclaration cached_declaration_{};
    FramePlan plan_{};
    bool cached_ = false;
    bool cache_hit_ = false;
};

} // namespace engine::render_detail
