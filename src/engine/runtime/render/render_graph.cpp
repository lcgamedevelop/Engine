#include "runtime/render/render_graph.hpp"

#include "runtime/performance/profiling.hpp"

#include <algorithm>
#include <array>

namespace engine::render_detail
{
namespace
{
using Producers = std::array<const GraphPassDesc*, graph_slot_count>;

bool collect_producers(const GraphDeclaration& declaration, Producers& producers,
                       GraphError& error) noexcept
{
    std::array<bool, graph_pass_count> registered{};
    for (const auto& pass : std::span(declaration.passes).first(declaration.pass_count))
    {
        if (pass.pass >= registered.size() || registered[pass.pass] ||
            pass.output >= declaration.slot_count)
        {
            error = {"Invalid or duplicate render graph pass declaration", pass.pass, pass.output};
            return false;
        }
        if (producers[pass.output] || declaration.imports[pass.output])
        {
            error = {"Render graph resource version has multiple producers", pass.pass,
                     pass.output};
            return false;
        }
        registered[pass.pass] = true;
        producers[pass.output] = &pass;
    }
    return true;
}

enum class Visit : std::uint8_t
{
    unseen,
    active,
    finished
};

// There is one visit per version and no more stack frames than there are resource slots.
bool visit(GraphSlot slot, GraphPass reader, const GraphDeclaration& declaration,
           const Producers& producers, std::array<Visit, graph_slot_count>& visited,
           GraphError& error) noexcept
{
    if (slot >= declaration.slot_count)
    {
        error = {"Required render graph input or output is missing", reader, slot};
        return false;
    }
    if (visited[slot] == Visit::active)
    {
        error = {"Cycle in render graph resource dependencies", reader, slot};
        return false;
    }
    if (visited[slot] == Visit::finished || declaration.imports[slot])
    {
        return true;
    }
    const auto* pass = producers[slot];
    if (pass == nullptr)
    {
        error = {"Render graph resource version has no producer or defined import", reader, slot};
        return false;
    }
    visited[slot] = Visit::active;
    for (const auto input : pass->reads)
    {
        if (input != graph_invalid_id &&
            !visit(input, pass->pass, declaration, producers, visited, error))
        {
            return false;
        }
    }
    visited[slot] = Visit::finished;
    return true;
}

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
    ENGINE_PROFILE_ZONE("Render.graphCompileMiss");
    if (declaration.pass_count > graph_pass_count || declaration.slot_count > graph_slot_count ||
        declaration.first_view > declaration.last_view)
    {
        error.reason = "Invalid render graph slice size or view reservation";
        return false;
    }
    Producers producers{};
    std::array<Visit, graph_slot_count> visited{};
    if (!collect_producers(declaration, producers, error) ||
        !visit(declaration.root, graph_invalid_id, declaration, producers, visited, error))
    {
        return false;
    }
    if (producers[declaration.root] == nullptr)
    {
        error = {"Render graph slice requires a written output", graph_invalid_id,
                 declaration.root};
        return false;
    }
    // The surrounding batch owns View order. A fixed View can only occur once in this
    // slice, and every live producer must precede its consumer in that physical order.
    for (const auto& pass : std::span(declaration.passes).first(declaration.pass_count))
    {
        if (visited[pass.output] == Visit::finished)
        {
            if (pass.view < declaration.first_view || pass.view > declaration.last_view ||
                pass.view >= key.max_views)
            {
                error = {"Render graph pass conflicts with fixed view ownership or device limit",
                         pass.pass, pass.output};
                return false;
            }
            plan.scheduled[plan.count++] = pass;
        }
    }
    const auto end = plan.scheduled.begin() + plan.count;
    std::sort(plan.scheduled.begin(), end,
              [](const auto& left, const auto& right) { return left.view < right.view; });
    auto available = declaration.imports;
    for (std::uint8_t position = 0; position < plan.count; ++position)
    {
        const auto& pass = plan.scheduled[position];
        if (position && plan.scheduled[position - 1].view == pass.view)
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
            if (!available[slot])
            {
                error = {"Fixed render graph view order would sample before its producer",
                         pass.pass, slot};
                return false;
            }
            record_use(plan, slot, position);
        }
        record_use(plan, pass.output, position);
        available[pass.output] = true;
    }
    return true;
}

bool validate_bindings(const FramePlan& plan, std::size_t slots, const FrameBindings& bindings,
                       GraphError& error) noexcept
{
    ENGINE_PROFILE_ZONE("Render.graphValidateAliases");
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

bool RenderGraph::compile(const GraphDeclaration& declaration, const FrameBindings& bindings,
                          GraphError& error) noexcept
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
        ENGINE_PROFILE_ZONE("Render.graphCacheLookup");
        hit = cached_ && cached_key_ == key_ && cached_declaration_ == declaration;
    }
    if (hit)
    {
        // Identical slot identities prove the same aliases and validity as the last success.
        if (bindings_ != bindings &&
            !validate_bindings(plan_, declaration.slot_count, bindings, error))
        {
            return false;
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
        cached_declaration_ = declaration;
        cached_ = true;
    }
    bindings_ = bindings;
    cache_hit_ = hit;
    phase_ = Phase::compiled;
    return true;
}

bool RenderGraph::consume(const FrameBindings& bindings, GraphError& error) noexcept
{
    ENGINE_PROFILE_ZONE("Render.graphRecheckBindings");
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
    phase_ = Phase::idle;
    cached_ = false;
    cache_hit_ = false;
    bindings_ = {};
    plan_ = {};
}
} // namespace engine::render_detail
