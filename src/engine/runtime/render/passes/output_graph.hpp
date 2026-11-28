#pragma once

#include "runtime/render/frame_graph_plan.hpp"
#include "runtime/render/render_observation.hpp"

#include <array>
#include <cstddef>
#include <span>

namespace engine::render_detail
{
enum class OutputPass : FramePassId
{
    upload_y,
    upload_chroma,
    upload_v,
    clear,
    scene,
    diffuse_capture,
    movie,
    ui,
    rvt_overlay,
    vsm_overlay,
    present,
    capture,
    count
};

struct OutputGraphKey
{
    RenderFrameKind kind = RenderFrameKind::clear;
    bool ui = false;
    bool rvt_overlay = false;
    bool vsm_overlay = false;
    bool diffuse_capture = false;
    bool screenshot = false;
    bool separate_chroma = false;
    bool operator==(const OutputGraphKey&) const = default;
};

// Successive versions of the display image share FrameOutput's BGRA8 target. A partial write
// (letterboxed movie, UI or overlay) loads the preceding version. Presentation samples the final
// image into the swapchain; readback samples that same image, including every displayed overlay.
[[nodiscard]] inline bool compile_output_graph(FrameGraphPlan& plan, const OutputGraphKey& key,
                                               FramePlanError& error) noexcept
{
    constexpr FrameResourceId cleared = 0;
    constexpr FrameResourceId scene_color = 1;
    constexpr FrameResourceId scene_state = 2;
    constexpr FrameResourceId y_plane = 3;
    constexpr FrameResourceId movie_color = 4;
    constexpr FrameResourceId ui_color = 5;
    constexpr FrameResourceId rvt_color = 6;
    constexpr FrameResourceId vsm_color = 7;
    constexpr FrameResourceId presented = 8;
    constexpr std::array scene_outputs{scene_color, scene_state};
    constexpr FrameResourceId chroma_plane = 9, v_plane = 10, decoded = 11;
    constexpr std::array movie_inputs{cleared, y_plane, chroma_plane, v_plane};
    constexpr std::array uploads{static_cast<FramePassId>(OutputPass::upload_y),
                                 static_cast<FramePassId>(OutputPass::upload_chroma),
                                 static_cast<FramePassId>(OutputPass::upload_v)};
    constexpr auto diffuse = static_cast<FramePassId>(OutputPass::diffuse_capture);
    constexpr auto present = static_cast<FramePassId>(OutputPass::present);
    using Resources = std::span<const FrameResourceId>;
    using Predecessors = std::span<const FramePassId>;
    std::array<FramePassDeclaration, static_cast<std::size_t>(OutputPass::count)> declarations;
    std::size_t count = 0;
    const auto add = [&](OutputPass pass, Resources reads, Resources writes,
                         Predecessors after = {}, bool side_effect = false)
    {
        declarations[count++] = {static_cast<FramePassId>(pass), reads, writes, after, side_effect};
    };
    const bool movie = key.kind == RenderFrameKind::movie;
    Resources color;
    if (movie)
    {
        add(OutputPass::upload_y, {&decoded, 1}, {&y_plane, 1});
        add(OutputPass::upload_chroma, {&decoded, 1}, {&chroma_plane, 1});
        if (key.separate_chroma)
        {
            add(OutputPass::upload_v, {&decoded, 1}, {&v_plane, 1});
        }
        // A partial upload must fail before even a clear enters this GPU frame.
        const auto num_planes = key.separate_chroma ? 3U : 2U;
        add(OutputPass::clear, {}, {&cleared, 1}, std::span(uploads).first(num_planes));
        add(OutputPass::movie, std::span(movie_inputs).first(num_planes + 1), {&movie_color, 1});
        color = {&movie_color, 1};
    }
    else
    {
        add(OutputPass::clear, {}, {&cleared, 1});
        // Scene collection also publishes the empty / no-camera outcome for scoped requests.
        add(OutputPass::scene, {&cleared, 1}, scene_outputs);
        color = {&scene_color, 1};
        Predecessors after_capture;
        if (key.diffuse_capture)
        {
            add(OutputPass::diffuse_capture, {&scene_state, 1}, {}, {}, true);
            after_capture = {&diffuse, 1};
        }
        const auto composite = [&](OutputPass pass, const FrameResourceId& output)
        {
            add(pass, color, {&output, 1}, after_capture);
            color = {&output, 1};
        };
        if (key.ui)
        {
            composite(OutputPass::ui, ui_color);
        }
        if (key.rvt_overlay)
        {
            composite(OutputPass::rvt_overlay, rvt_color);
        }
        if (key.vsm_overlay)
        {
            composite(OutputPass::vsm_overlay, vsm_color);
        }
    }
    const Predecessors after_capture =
        !movie && key.diffuse_capture ? Predecessors{&diffuse, 1} : Predecessors{};
    add(OutputPass::present, color, {&presented, 1}, after_capture);
    if (key.screenshot)
    {
        add(OutputPass::capture, color, {}, {&present, 1}, true);
    }
    const Resources imports = movie ? Resources{&decoded, 1} : Resources{};
    return plan.compile(std::span{declarations}.first(count), imports, {&presented, 1}, error);
}
} // namespace engine::render_detail
