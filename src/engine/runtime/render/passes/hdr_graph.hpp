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
[[nodiscard]] consteval GraphDeclaration make_hdr_declaration(bool bloom_enabled) noexcept
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
inline constexpr auto hdr_without_bloom = make_hdr_declaration(false);
inline constexpr auto hdr_with_bloom = make_hdr_declaration(true);

[[nodiscard]] inline constexpr const GraphDeclaration&
declare_hdr_graph(bool bloom_enabled) noexcept
{
    return bloom_enabled ? hdr_with_bloom : hdr_without_bloom;
}

[[nodiscard]] inline bool compile_hdr_graph(RenderGraph& graph, bool bloom_enabled,
                                            const FrameBindings& bindings,
                                            GraphError& error) noexcept
{
    return bloom_enabled ? graph.compile<hdr_with_bloom>(bindings, error)
                         : graph.compile<hdr_without_bloom>(bindings, error);
}
} // namespace engine::render_detail
