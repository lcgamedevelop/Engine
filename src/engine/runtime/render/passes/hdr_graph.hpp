#pragma once

#include "runtime/render/render_constants.hpp"
#include "runtime/render/render_graph.hpp"

namespace engine::render_detail
{
struct HdrSlot
{
    enum : GraphSlot
    {
        scene,
        black,
        bright,
        blur,
        output,
        count
    };
};

struct HdrPass
{
    enum : GraphPass
    {
        bright,
        blur,
        resolve,
        count
    };
};

// The resolve shader always samples two textures. Black supplies its second input when Bloom
// is off. The unused extraction and blur are pruned from this same declaration.
[[nodiscard]] inline constexpr GraphDeclaration declare_hdr_graph(bool bloom_enabled) noexcept
{
    GraphDeclaration declaration;
    declaration.pass_count = HdrPass::count;
    declaration.slot_count = HdrSlot::count;
    declaration.first_view = hdr_bright_view;
    declaration.last_view = hdr_resolve_view;
    declaration.imports[HdrSlot::scene] = true;
    declaration.imports[HdrSlot::black] = true;
    declaration.passes[HdrPass::bright] = {
        HdrPass::bright, hdr_bright_view, HdrSlot::bright, {HdrSlot::scene, graph_invalid_id}};
    declaration.passes[HdrPass::blur] = {
        HdrPass::blur, hdr_blur_view, HdrSlot::blur, {HdrSlot::bright, graph_invalid_id}};
    declaration.passes[HdrPass::resolve] = {
        HdrPass::resolve,
        hdr_resolve_view,
        HdrSlot::output,
        {HdrSlot::scene, bloom_enabled ? HdrSlot::blur : HdrSlot::black}};
    declaration.root = HdrSlot::output;
    return declaration;
}
} // namespace engine::render_detail
