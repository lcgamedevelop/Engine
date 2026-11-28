#pragma once

#include "runtime/render/frame_graph_plan.hpp"

namespace engine::render_detail::ao_graph
{
// The ASSAO Q2 programs use four deinterleaved depth textures and four useful mips per texture.
// Each generate dispatch writes one slice of the four-layer RG8 result.
inline constexpr std::uint16_t layers = 4;
inline constexpr std::uint16_t mip_levels = 4;
inline constexpr FramePassId prepare = 0;
inline constexpr FramePassId first_mip = prepare + 1;
inline constexpr FramePassId first_generate = first_mip + mip_levels - 1;
inline constexpr FramePassId blur_out = first_generate + layers;
inline constexpr FramePassId blur_back = blur_out + 1;
inline constexpr FramePassId apply = blur_back + 1;

inline constexpr FrameResourceId source = 0;
inline constexpr FrameResourceId normal = 1;
inline constexpr FrameResourceId first_depth = normal + 1;
inline constexpr FrameResourceId first_generated = first_depth + layers * mip_levels;
inline constexpr FrameResourceId first_blurred = first_generated + layers;
inline constexpr FrameResourceId first_final = first_blurred + layers;
inline constexpr FrameResourceId result = first_final + layers;

inline constexpr auto versions = []
{
    std::array<FrameResourceId, result + 1> ids{};
    for (FrameResourceId i = 0; i < ids.size(); ++i)
    {
        ids[i] = i;
    }
    return ids;
}();

inline constexpr auto generate_reads = []
{
    std::array<std::array<FrameResourceId, mip_levels + 1>, layers> reads{};
    for (std::uint16_t layer = 0; layer < layers; ++layer)
    {
        reads[layer][0] = normal;
        for (std::uint16_t mip = 0; mip < mip_levels; ++mip)
        {
            reads[layer][1 + mip] =
                static_cast<FrameResourceId>(first_depth + mip * layers + layer);
        }
    }
    return reads;
}();

inline constexpr auto declarations = []
{
    std::array<FramePassDeclaration, apply + 1> passes{};
    const std::span ids{versions};
    passes[prepare] = {prepare, ids.subspan(source, 1), ids.subspan(normal, 1 + layers)};
    for (std::uint16_t mip = 1; mip < mip_levels; ++mip)
    {
        const FramePassId pass = first_mip + mip - 1;
        const auto output = first_depth + mip * layers;
        passes[pass] = {pass, ids.subspan(output - layers, layers), ids.subspan(output, layers)};
    }
    for (std::uint16_t layer = 0; layer < layers; ++layer)
    {
        const FramePassId pass = first_generate + layer;
        passes[pass] = {pass, generate_reads[layer], ids.subspan(first_generated + layer, 1)};
    }
    // The second blur returns to the generate allocation. Distinct version IDs and the
    // intervening read of every generated slice preserve that storage's write-after-read order.
    passes[blur_out] = {blur_out, ids.subspan(first_generated, layers),
                        ids.subspan(first_blurred, layers)};
    passes[blur_back] = {blur_back, ids.subspan(first_blurred, layers),
                         ids.subspan(first_final, layers)};
    passes[apply] = {apply, ids.subspan(first_final, layers), ids.subspan(result, 1)};
    return passes;
}();
} // namespace engine::render_detail::ao_graph
