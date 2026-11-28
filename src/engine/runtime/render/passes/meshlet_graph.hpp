#pragma once

#include "runtime/render/frame_graph_plan.hpp"

namespace engine::render_detail
{
enum class MeshletStage : FramePassId
{
    copy_indices,
    upload_descriptors,
    upload_inputs,
    upload_jobs,
    clear_counters,
    cull,
    compact,
    read_counters,
    count
};

struct MeshletGraphShape
{
    bool copy_indices = false, upload_descriptors = false, upload_inputs = false,
         upload_jobs = false;
    bool draws = false, instances = false, compact = false, sample = false, reuse = false;
    bool operator==(const MeshletGraphShape&) const = default;
};

// IDs describe buffer ranges. Source and compacted indices occupy disjoint regions of the arena.
// The base and compacted indirect commands likewise occupy separate ranges of the same buffer.
[[nodiscard]] inline bool compile_meshlet_graph(FrameGraphPlan& plan, MeshletGraphShape shape,
                                                FramePlanError& error)
{
    constexpr FrameResourceId geometry_pages = 0, resident_sources = 1, source_indices = 2;
    constexpr FrameResourceId cpu_descriptors = 3, descriptors = 4, cpu_inputs = 5, inputs = 6;
    constexpr FrameResourceId cpu_jobs = 7, jobs = 8, cleared = 9, culled = 10, compacted = 11;
    constexpr FrameResourceId commands = 12, visible_instances = 13, compact_commands = 14,
                              compact_indices = 15;
    std::array<FramePassDeclaration, static_cast<std::size_t>(MeshletStage::count)> passes{};
    std::array<FrameResourceId, 16> imports{};
    std::array<FrameResourceId, 4> roots{};
    std::size_t count = 0, import_count = 0, root_count = 0;
    const auto add = [&](MeshletStage stage, std::span<const FrameResourceId> reads,
                         std::span<const FrameResourceId> writes, bool side_effect = false)
    { passes[count++] = {static_cast<FramePassId>(stage), reads, writes, {}, side_effect}; };
    const auto upload =
        [&](MeshletStage stage, const FrameResourceId& cpu, const FrameResourceId& gpu, bool dirty)
    {
        if (dirty)
        {
            imports[import_count++] = cpu;
            add(stage, {&cpu, 1}, {&gpu, 1});
        }
        else
        {
            imports[import_count++] = gpu;
        }
    };
    const std::array copy_reads{geometry_pages, resident_sources};
    if (shape.copy_indices)
    {
        imports[import_count++] = geometry_pages;
        imports[import_count++] = resident_sources;
        add(MeshletStage::copy_indices, copy_reads, {&source_indices, 1}, true);
    }
    else if (shape.compact && !shape.reuse)
    {
        imports[import_count++] = source_indices;
    }
    if (shape.draws && !shape.reuse)
    {
        upload(MeshletStage::upload_descriptors, cpu_descriptors, descriptors,
               shape.upload_descriptors);
        upload(MeshletStage::upload_inputs, cpu_inputs, inputs, shape.upload_inputs);
    }
    if (shape.compact && !shape.reuse)
    {
        upload(MeshletStage::upload_jobs, cpu_jobs, jobs, shape.upload_jobs);
    }
    if (shape.sample)
    {
        add(MeshletStage::clear_counters, {}, {&cleared, 1});
    }
    const std::array cull_reads{descriptors, inputs, cleared};
    std::array<FrameResourceId, 3> cull_writes{commands};
    std::size_t cull_outputs = 1;
    if (shape.draws)
    {
        roots[root_count++] = commands;
        if (shape.instances)
        {
            roots[root_count++] = visible_instances;
            cull_writes[cull_outputs++] = visible_instances;
        }
        if (shape.sample)
        {
            cull_writes[cull_outputs++] = culled;
        }
        if (shape.reuse)
        {
            imports[import_count++] = commands;
            if (shape.instances)
            {
                imports[import_count++] = visible_instances;
            }
        }
        else
        {
            add(MeshletStage::cull, std::span(cull_reads).first(shape.sample ? 3 : 2),
                std::span(cull_writes).first(cull_outputs));
        }
    }
    const std::array compact_reads{jobs, commands, source_indices, culled};
    const std::array compact_writes{compact_commands, compact_indices, compacted};
    if (shape.compact)
    {
        roots[root_count++] = compact_commands;
        roots[root_count++] = compact_indices;
        if (shape.reuse)
        {
            imports[import_count++] = compact_commands;
            imports[import_count++] = compact_indices;
        }
        else
        {
            add(MeshletStage::compact, std::span(compact_reads).first(shape.sample ? 4 : 3),
                std::span(compact_writes).first(shape.sample ? 3 : 2));
        }
    }
    if (shape.sample)
    {
        const auto& counters = shape.compact ? compacted : (shape.draws ? culled : cleared);
        add(MeshletStage::read_counters, {&counters, 1}, {}, true);
    }
    return plan.compile(std::span(passes).first(count), std::span(imports).first(import_count),
                        std::span(roots).first(root_count), error);
}
} // namespace engine::render_detail
