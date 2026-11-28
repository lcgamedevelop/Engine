#include "runtime/render/frame_graph_plan.hpp"

#include <algorithm>
#include <bitset>

namespace engine
{
namespace
{
using PassSet = std::bitset<frame_pass_capacity>;
using PassTable = std::array<FramePassId, frame_pass_capacity>;
using ResourceTable = std::array<FramePassId, frame_resource_capacity>;
using Edges = std::array<PassSet, frame_pass_capacity>;
using Declarations = std::span<const FramePassDeclaration>;

bool index_declarations(Declarations passes, std::span<const FrameResourceId> imports,
                        ResourceTable& producers, std::bitset<frame_resource_capacity>& imported,
                        Edges& predecessors, FramePlanError& error)
{
    PassTable indices;
    indices.fill(frame_invalid_id);
    producers.fill(frame_invalid_id);
    for (const auto resource : imports)
    {
        if (resource >= frame_resource_capacity || imported[resource])
        {
            error = {"Invalid or duplicate frame resource import", frame_invalid_id, resource};
            return false;
        }
        imported[resource] = true;
    }
    for (std::size_t i = 0; i < passes.size(); ++i)
    {
        const auto& pass = passes[i];
        if (pass.id >= frame_pass_capacity || indices[pass.id] != frame_invalid_id)
        {
            error = {"Invalid or duplicate frame pass ID", pass.id};
            return false;
        }
        indices[pass.id] = static_cast<FramePassId>(i);
        for (const auto resource : pass.writes)
        {
            if (resource >= frame_resource_capacity)
            {
                error = {"Frame output resource ID exceeds capacity", pass.id, resource};
                return false;
            }
            if (imported[resource] || producers[resource] != frame_invalid_id)
            {
                const auto writer = producers[resource];
                error = {"Frame resource version has multiple producers", pass.id, resource,
                         writer == frame_invalid_id ? frame_invalid_id : passes[writer].id};
                return false;
            }
            producers[resource] = static_cast<FramePassId>(i);
        }
        for (const auto resource : pass.reads)
        {
            if (resource >= frame_resource_capacity)
            {
                error = {"Frame input resource ID exceeds capacity", pass.id, resource};
                return false;
            }
        }
    }
    for (std::size_t i = 0; i < passes.size(); ++i)
    {
        for (const auto before : passes[i].after)
        {
            if (before >= frame_pass_capacity || indices[before] == frame_invalid_id)
            {
                error = {"Frame ordering references an unknown pass", passes[i].id,
                         frame_invalid_id, before};
                return false;
            }
            predecessors[i][indices[before]] = true;
        }
    }
    return true;
}

bool mark_required(Declarations passes, const ResourceTable& producers,
                   const std::bitset<frame_resource_capacity>& imported,
                   std::span<const FrameResourceId> roots, PassSet& live, Edges& predecessors,
                   FramePlanError& error)
{
    PassTable pending;
    std::size_t count = 0;
    const auto require = [&](FramePassId index)
    {
        if (!live[index])
        {
            live[index] = true;
            pending[count++] = index;
        }
    };
    for (const auto root : roots)
    {
        if (root >= frame_resource_capacity ||
            (!imported[root] && producers[root] == frame_invalid_id))
        {
            error = {"Required frame output has no writer or import", frame_invalid_id, root};
            return false;
        }
        if (!imported[root])
        {
            require(producers[root]);
        }
    }
    for (std::size_t i = 0; i < passes.size(); ++i)
    {
        if (passes[i].side_effect)
        {
            require(static_cast<FramePassId>(i));
        }
    }
    while (count > 0)
    {
        const auto reader = pending[--count];
        for (const auto resource : passes[reader].reads)
        {
            if (imported[resource])
            {
                continue;
            }
            const auto writer = producers[resource];
            if (writer == frame_invalid_id)
            {
                error = {"Live frame input has no writer or import", passes[reader].id, resource};
                return false;
            }
            predecessors[reader][writer] = true;
            require(writer);
        }
    }
    return true;
}

void project_order(std::size_t count, const PassSet& live, Edges& predecessors)
{
    // Eliminate only dead intermediates. Their pure order edges survive between live
    // endpoints, but reading their inputs never recruits otherwise discarded work.
    for (std::size_t middle = 0; middle < count; ++middle)
    {
        if (live[middle])
        {
            continue;
        }
        for (std::size_t end = 0; end < count; ++end)
        {
            if (predecessors[end][middle])
            {
                predecessors[end] |= predecessors[middle];
            }
        }
    }
    for (std::size_t i = 0; i < count; ++i)
    {
        predecessors[i] &= live;
    }
}

void describe_cycle(Declarations passes, const Edges& predecessors, const PassSet& remaining,
                    const ResourceTable& producers, std::span<FramePassId> path,
                    FramePlanError& error)
{
    // Every remaining node has a remaining predecessor. Following them must revisit a
    // node within the fixed pass limit. Return the cycle in producer-to-consumer order.
    PassTable position;
    position.fill(frame_invalid_id);
    std::size_t index = 0;
    while (!remaining[index])
    {
        ++index;
    }
    std::size_t length = 0;
    while (position[index] == frame_invalid_id)
    {
        position[index] = static_cast<FramePassId>(length);
        path[length++] = passes[index].id;
        std::size_t before = 0;
        while (!(remaining[before] && predecessors[index][before]))
        {
            ++before;
        }
        index = before;
    }
    path[length++] = passes[index].id;
    const auto cycle = path.subspan(position[index], length - position[index]);
    std::ranges::reverse(cycle);
    const auto last_producer = cycle[cycle.size() - 2];
    auto reason_resource = frame_invalid_id;
    for (const auto resource : passes[index].reads)
    {
        if (producers[resource] != frame_invalid_id &&
            passes[producers[resource]].id == last_producer)
        {
            reason_resource = resource;
            break;
        }
    }
    error = {"Cycle in live frame pass dependencies", passes[index].id, reason_resource,
             last_producer, cycle};
}
} // namespace

bool FrameGraphPlan::compile(Declarations passes, std::span<const FrameResourceId> imports,
                             std::span<const FrameResourceId> roots, FramePlanError& error) noexcept
{
    error = {};
    if (state_ == State::executing)
    {
        error.reason = "Cannot replace an executing frame plan";
        return false;
    }
    state_ = State::invalid;
    count_ = 0;
    if (passes.size() > frame_pass_capacity)
    {
        error.reason = "Frame graph exceeds pass capacity";
        return false;
    }
    ResourceTable producers;
    std::bitset<frame_resource_capacity> imported;
    Edges predecessors{};
    PassSet remaining;
    if (!index_declarations(passes, imports, producers, imported, predecessors, error) ||
        !mark_required(passes, producers, imported, roots, remaining, predecessors, error))
    {
        return false;
    }
    project_order(passes.size(), remaining, predecessors);
    std::uint16_t scheduled = 0;
    while (remaining.any())
    {
        std::size_t next = 0;
        while (next < passes.size() && (!remaining[next] || (predecessors[next] & remaining).any()))
        {
            ++next;
        }
        if (next == passes.size())
        {
            describe_cycle(passes, predecessors, remaining, producers, cycle_, error);
            return false;
        }
        scheduled_[scheduled++] = passes[next].id;
        remaining[next] = false;
    }
    count_ = scheduled;
    state_ = State::ready;
    return true;
}

bool FrameGraphPlan::begin_cached_frame() noexcept
{
    if (state_ != State::consumed)
    {
        return false;
    }
    state_ = State::ready;
    return true;
}
} // namespace engine
