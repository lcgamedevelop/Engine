#include "runtime/render/render_graph.hpp"

#include "runtime/performance/profiling.hpp"
#include "runtime/render/frame_graph_plan.hpp"

#include <algorithm>
#include <array>

namespace engine::render_detail
{
namespace
{
void record_use(FramePlan& plan, GraphSlot slot, std::uint8_t position) noexcept
{
    auto& interval = plan.intervals[slot];
    if (interval.first == GraphInterval::unused)
    {
        interval.first = position;
    }
    interval.last = position;
}

bool build_plan(const FrameKey& key, const GraphDeclaration& declaration, FramePlan& plan,
                GraphError& error) noexcept
{
#if defined(ENGINE_ENABLE_TRACY_FUNCTIONS)
    ENGINE_PROFILE_ZONE("Render.graphCompileMiss");
#endif
    if (declaration.pass_count > graph_pass_count || declaration.slot_count > graph_slot_count ||
        declaration.first_view > declaration.last_view)
    {
        error.reason = "Invalid render graph slice size or view reservation";
        return false;
    }
    if (declaration.root >= declaration.slot_count || declaration.imports[declaration.root])
    {
        error = {"Render graph slice requires a written output", graph_invalid_id,
                 declaration.root};
        return false;
    }

    // Widen the compact per-effect declaration for the frame compiler. Only the resulting
    // segment order and lifetimes survive this cache miss.
    std::array<FrameResourceId, graph_slot_count> imports;
    std::size_t import_count = 0;
    for (GraphSlot slot = 0; slot < declaration.slot_count; ++slot)
    {
        if (declaration.imports[slot])
        {
            imports[import_count++] = slot;
        }
    }
    std::array<FramePassDeclaration, graph_pass_count> records{};
    std::array<std::array<FrameResourceId, 2>, graph_pass_count> reads{};
    std::array<FrameResourceId, graph_pass_count> writes{};
    std::array<const GraphPassDesc*, graph_pass_count> by_id{};
    std::array<std::uint16_t, graph_slot_count> writer_views{};
    const auto passes = std::span(records).first(declaration.pass_count);
    for (std::size_t i = 0; i < passes.size(); ++i)
    {
        const auto& pass = declaration.passes[i];
        if (pass.pass >= graph_pass_count || pass.output >= declaration.slot_count)
        {
            error = {"Invalid render graph pass declaration", pass.pass, pass.output};
            return false;
        }
        by_id[pass.pass] = &pass;
        writer_views[pass.output] = pass.view;
        std::size_t count = 0;
        for (const auto slot : pass.reads)
        {
            if (slot != graph_invalid_id)
            {
                reads[i][count++] = slot;
            }
        }
        writes[i] = pass.output;
        passes[i] = {pass.pass, std::span(reads[i]).first(count), {&writes[i], 1}};
    }
    // Independent ready segments must follow their fixed GPU View order. The frame compiler
    // retains declaration order between ready passes and validates the version dependency graph.
    std::ranges::sort(passes, {},
                      [&](const FramePassDeclaration& pass) { return by_id[pass.id]->view; });
    FrameGraphPlan compiled;
    FramePlanError problem;
    const FrameResourceId root = declaration.root;
    if (!compiled.compile(passes, std::span(imports).first(import_count), {&root, 1}, problem))
    {
        const auto pass = static_cast<GraphPass>(problem.pass);
        const auto slot = problem.resource == frame_invalid_id && pass < by_id.size() && by_id[pass]
                              ? by_id[pass]->output
                              : static_cast<GraphSlot>(problem.resource);
        error = {problem.reason, pass, slot};
        return false;
    }
    for (const auto id : compiled.passes())
    {
        const auto& pass = *by_id[id];
        if (pass.view < declaration.first_view || pass.view > declaration.last_view ||
            pass.view >= key.max_views)
        {
            error = {"Render graph pass conflicts with fixed view ownership or device limit",
                     pass.pass, pass.output};
            return false;
        }
        if (plan.count && plan.scheduled[plan.count - 1].view == pass.view)
        {
            error = {"Render graph segments share a fixed view", pass.pass, pass.output};
            return false;
        }
        for (const auto slot : pass.reads)
        {
            if (slot == graph_invalid_id)
            {
                continue;
            }
            if (!declaration.imports[slot] && writer_views[slot] >= pass.view)
            {
                error = {"Fixed render graph view order would sample before its producer",
                         pass.pass, slot};
                return false;
            }
            record_use(plan, slot, plan.count);
        }
        record_use(plan, pass.output, plan.count);
        plan.scheduled[plan.count++] = pass;
    }
    return true;
}

bool validate_bindings(const FramePlan& plan, std::size_t slots, const FrameBindings& bindings,
                       GraphError& error) noexcept
{
#if defined(ENGINE_ENABLE_TRACY_FUNCTIONS)
    ENGINE_PROFILE_ZONE("Render.graphValidateAliases");
#endif
    for (std::size_t slot = 0; slot < slots; ++slot)
    {
        const auto first = plan.intervals[slot].first;
        if (first == GraphInterval::unused)
        {
            continue;
        }
        const auto texture = bindings.textures[slot];
        if (texture == graph_invalid_texture)
        {
            error = {"Render graph resource has no valid texture binding",
                     plan.scheduled[first].pass, static_cast<GraphSlot>(slot)};
            return false;
        }
        // Each live image in this slice requires independent physical storage;
        // rechecking this relation also validates structural cache hits with new handles.
        for (std::size_t other = 0; other < slot; ++other)
        {
            if (plan.intervals[other].first != GraphInterval::unused &&
                bindings.textures[other] == texture)
            {
                error = {"Live render graph resource slots alias the same texture",
                         plan.scheduled[first].pass, static_cast<GraphSlot>(slot)};
                return false;
            }
        }
    }
    return true;
}
} // namespace

bool RenderGraph::prepare_output(const OutputGraphKey& key, FramePlanError& error) noexcept
{
    if (output_cached_ && output_key_ == key && output_plan_.begin_cached_frame())
    {
        return true;
    }
    output_cached_ = compile_output_graph(output_plan_, key, error);
    if (output_cached_)
    {
        output_key_ = key;
    }
    return output_cached_;
}

bool RenderGraph::begin(const FrameKey& key) noexcept
{
    phase_ = Phase::idle;
    cache_hit_ = false;
    if (key.width == 0 || key.height == 0 || key.max_views == 0 || key.samples == 0)
    {
        return false;
    }
    key_ = key;
    phase_ = Phase::declaring;
    return true;
}

bool RenderGraph::compile_frame(const GraphDeclaration& declaration, bool immutable,
                                const FrameBindings& bindings, GraphError& error) noexcept
{
    error = {};
    if (phase_ != Phase::declaring)
    {
        error.reason = "Render graph is not open for compilation";
        return false;
    }
    phase_ = Phase::idle;
    bool hit;
    {
#if defined(ENGINE_ENABLE_TRACY_FUNCTIONS)
        ENGINE_PROFILE_ZONE("Render.graphCacheLookup");
#endif
        const auto& previous = cached_immutable_ ? *cached_immutable_ : cached_declaration_;
        hit = cached_ && cached_key_ == key_ &&
              (immutable && cached_immutable_ ? cached_immutable_ == &declaration
                                              : previous == declaration);
    }
    if (hit)
    {
        // Identical slot identities prove the same aliases and validity as the last success.
        if (bindings_ != bindings)
        {
            if (!validate_bindings(plan_, declaration.slot_count, bindings, error))
            {
                return false;
            }
            bindings_ = bindings;
        }
    }
    else
    {
        FramePlan candidate;
        if (!build_plan(key_, declaration, candidate, error) ||
            !validate_bindings(candidate, declaration.slot_count, bindings, error))
        {
            return false;
        }
        plan_ = candidate;
        cached_key_ = key_;
        cached_immutable_ = immutable ? &declaration : nullptr;
        if (!immutable)
        {
            cached_declaration_ = declaration;
        }
        bindings_ = bindings;
        cached_ = true;
    }
    cache_hit_ = hit;
    phase_ = Phase::compiled;
    return true;
}

bool RenderGraph::consume(const FrameBindings& bindings, GraphError& error) noexcept
{
#if defined(ENGINE_ENABLE_TRACY_FUNCTIONS)
    ENGINE_PROFILE_ZONE("Render.graphRecheckBindings");
#endif
    error = {};
    if (phase_ != Phase::compiled)
    {
        error.reason = "Render graph requires a compiled, unconsumed frame";
        return false;
    }
    phase_ = Phase::idle;
    if (bindings_ != bindings)
    {
        error.reason = "Render graph frame bindings changed after compilation";
        return false;
    }
    return true;
}

void RenderGraph::reset() noexcept
{
    output_cached_ = false;
    phase_ = Phase::idle;
    cached_ = false;
    cached_immutable_ = nullptr;
    cache_hit_ = false;
    bindings_ = {};
    plan_ = {};
}
} // namespace engine::render_detail
