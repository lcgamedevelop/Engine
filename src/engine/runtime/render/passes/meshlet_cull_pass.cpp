#include "runtime/render/passes/meshlet_cull_pass.hpp"

#include "runtime/performance/function_profile.hpp"
#include "runtime/performance/profiling.hpp"
#include "runtime/render/render_constants.hpp"
#include "runtime/render/render_math.hpp"
#include "runtime/render/render_submission.hpp"
#include "runtime/render/render_system.hpp"
#include "runtime/render/resources/gpu_program.hpp"
#include "runtime/render/scene_draw.hpp"

#include "cs_meshlet_clear.bin.h"
#include "cs_meshlet_compact_indices.bin.h"
#include "cs_meshlet_cull.bin.h"

#include <algorithm>
#include <bit>
#include <bx/math.h>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <utility>

namespace engine::render_detail
{
namespace
{
constexpr std::uint32_t singleton_clusters_per_command = 8;
constexpr std::uint32_t singleton_commands_per_group = 4;
static_assert(singleton_clusters_per_command * singleton_commands_per_group == 32);
constexpr std::uint32_t maximum_index_region_capacity = 3 * 1024 * 1024;
constexpr std::uint32_t index_compaction_capacity = 4096;
constexpr std::uint32_t index_span_capacity = index_compaction_capacity;
constexpr std::uint32_t minimum_compaction_commands = 4;
constexpr std::uint32_t maximum_compaction_commands = 64;

std::array<std::uint64_t, 5> index_source_key(const GpuGeometry& geometry, std::size_t lod,
                                              std::uint32_t first)
{
    ENGINE_PROFILE_FUNCTION();
    const auto id = geometry.resource.handle();
    return {id.owner, id.id, id.generation, lod, first};
}

bool cone_safe(const float* model, schema::CullMode cull)
{
    ENGINE_PROFILE_FUNCTION();
    if (cull != schema::CullMode::back || linear_determinant(model) <= 0)
    {
        return false;
    }
    const auto dot = [model](std::size_t a, std::size_t b)
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        return static_cast<double>(model[a * 4]) * model[b * 4] +
               static_cast<double>(model[a * 4 + 1]) * model[b * 4 + 1] +
               static_cast<double>(model[a * 4 + 2]) * model[b * 4 + 2];
    };
    const double length = dot(0, 0);
    const double tolerance = length * 1.0e-6;
    return length > 1.0e-12 && std::abs(dot(1, 1) - length) <= tolerance &&
           std::abs(dot(2, 2) - length) <= tolerance && std::abs(dot(0, 1)) <= tolerance &&
           std::abs(dot(0, 2)) <= tolerance && std::abs(dot(1, 2)) <= tolerance;
}

bgfx::VertexLayout instance_layout()
{
    ENGINE_PROFILE_FUNCTION();
    bgfx::VertexLayout layout;
    layout.begin();
    for (auto attribute :
         {bgfx::Attrib::TexCoord0, bgfx::Attrib::TexCoord1, bgfx::Attrib::TexCoord2,
          bgfx::Attrib::TexCoord3, bgfx::Attrib::TexCoord4, bgfx::Attrib::TexCoord5,
          bgfx::Attrib::TexCoord6})
    {
        layout.add(attribute, 4, bgfx::AttribType::Float);
    }
    layout.end();
    return layout;
}

[[nodiscard]] bool ensure_buffer(bgfx::DynamicVertexBufferHandle& buffer, std::uint32_t& capacity,
                                 std::uint32_t required, std::uint32_t maximum,
                                 const bgfx::VertexLayout& layout, std::uint16_t flags)
{
    ENGINE_PROFILE_FUNCTION();
    if (required <= capacity)
    {
        return true;
    }
    const auto count = std::max(required, std::min(maximum, capacity * 2));
    const auto next = bgfx::createDynamicVertexBuffer(count, layout, flags);
    if (!bgfx::isValid(next))
    {
        return false;
    }
    bgfx::destroy(buffer);
    buffer = next;
    capacity = count;
    return true;
}
} // namespace

bool meshlet_supported() noexcept
{
    ENGINE_PROFILE_FUNCTION();
    const auto* caps = bgfx::getCaps();
    constexpr auto required = BGFX_CAPS_COMPUTE | BGFX_CAPS_DRAW_INDIRECT;
    return caps && (caps->supported & required) == required && caps->limits.maxInstanceData >= 7 &&
           caps->limits.maxComputeBindings >= 5;
}

bool meshlet_index_compaction_supported() noexcept
{
    ENGINE_PROFILE_FUNCTION();
    const auto* caps = bgfx::getCaps();
    return meshlet_supported() && (caps->supported & BGFX_CAPS_DRAW_INDIRECT_COUNT) == 0 &&
           caps->limits.maxBlits >= 8;
}

MeshletCullPass::~MeshletCullPass() noexcept
{
    ENGINE_PROFILE_FUNCTION();
    release_gpu();
}

bool MeshletCullPass::prepare(const RenderConfig& config)
{
    ENGINE_PROFILE_FUNCTION();
    const auto capacity = config.maximum_meshlet_candidates;
    if (!meshlet_supported() || capacity == 0 || capacity > UINT32_MAX / (8 * 4 * sizeof(float)) ||
        capacity > UINT32_MAX / 124)
    {
        return false;
    }
    if (capacity_ == capacity && bgfx::isValid(program_))
    {
        return true;
    }
    release_gpu();
    const auto shader = bgfx::createShader(bgfx::copy(cs_meshlet_cull, sizeof(cs_meshlet_cull)));
    if (!bgfx::isValid(shader))
    {
        return false;
    }
    program_ = bgfx::createProgram(shader, true);
    const auto clear_shader =
        bgfx::createShader(bgfx::copy(cs_meshlet_clear, sizeof(cs_meshlet_clear)));
    if (bgfx::isValid(clear_shader))
    {
        clear_ = bgfx::createProgram(clear_shader, true);
    }
    parameters_ = bgfx::createUniform("u_meshlet_parameters", bgfx::UniformType::Vec4);
    planes_ = bgfx::createUniform("u_meshlet_planes", bgfx::UniformType::Vec4, 6);
    inputs_ =
        bgfx::createDynamicVertexBuffer(1, GpuGeometry::vector_layout(), BGFX_BUFFER_COMPUTE_READ);
    instances_ = bgfx::createDynamicVertexBuffer(1, instance_layout(), BGFX_BUFFER_COMPUTE_WRITE);
    descriptors_ =
        bgfx::createDynamicVertexBuffer(1, GpuGeometry::vector_layout(), BGFX_BUFFER_COMPUTE_READ);
    commands_ = bgfx::createIndirectBuffer(1);
    const std::array<std::uint32_t, 4> zero{};
    counters_ = bgfx::createIndexBuffer(bgfx::copy(zero.data(), sizeof(zero)),
                                        BGFX_BUFFER_INDEX32 | BGFX_BUFFER_COMPUTE_READ_WRITE);
    if (!bgfx::isValid(program_) || !bgfx::isValid(clear_) || !bgfx::isValid(parameters_) ||
        !bgfx::isValid(planes_) || !bgfx::isValid(inputs_) || !bgfx::isValid(instances_) ||
        !bgfx::isValid(commands_) || !bgfx::isValid(counters_) || !bgfx::isValid(descriptors_))
    {
        release_gpu();
        return false;
    }
    capacity_ = capacity;
    input_capacity_ = instance_capacity_ = command_capacity_ = descriptor_capacity_ = 1;
    bgfx::setViewName(meshlet_clear_view, "Meshlet counters");
    bgfx::setViewName(meshlet_cull_view, "Meshlet visibility");
    bgfx::setViewMode(meshlet_clear_view, bgfx::ViewMode::Sequential);
    bgfx::setViewMode(meshlet_cull_view, bgfx::ViewMode::Sequential);
    return true;
}

bool MeshletCullPass::prepare_index_compaction()
{
    ENGINE_PROFILE_FUNCTION();
    if (std::exchange(index_compaction_attempted_, true))
    {
        return bgfx::isValid(index_compaction_);
    }
    if (!meshlet_index_compaction_supported())
    {
        return false;
    }
    const auto shader = bgfx::createShader(
        bgfx::copy(cs_meshlet_compact_indices, sizeof(cs_meshlet_compact_indices)));
    if (!bgfx::isValid(shader))
    {
        return false;
    }
    index_compaction_ = bgfx::createProgram(shader, true);
    index_compaction_parameters_ =
        bgfx::createUniform("u_meshlet_index_compaction", bgfx::UniformType::Vec4);
    index_compaction_jobs_ =
        bgfx::createDynamicVertexBuffer(index_compaction_capacity + index_span_capacity,
                                        GpuGeometry::vector_layout(), BGFX_BUFFER_COMPUTE_READ);
    if (!bgfx::isValid(index_compaction_) || !bgfx::isValid(index_compaction_parameters_) ||
        !bgfx::isValid(index_compaction_jobs_))
    {
        release_index_compaction();
        return false;
    }
    bgfx::setViewName(meshlet_index_upload_view, "Meshlet source index copy");
    bgfx::setViewName(meshlet_index_compact_view, "Meshlet visible index compaction");
    bgfx::setViewMode(meshlet_index_compact_view, bgfx::ViewMode::Sequential);
    return true;
}

std::optional<std::uint32_t> MeshletCullPass::cache_index_source(const GpuGeometry& geometry,
                                                                 std::size_t lod,
                                                                 std::uint32_t first,
                                                                 std::uint32_t count,
                                                                 std::uint32_t& remaining_copies)
{
    ENGINE_PROFILE_FUNCTION();
    const auto key = index_source_key(geometry, lod, first);
    const auto existing = index_sources_.find(key);
    if (existing != index_sources_.end())
    {
        return existing->second;
    }
    // Buffer blits take four-byte aligned regions. Include the adjacent halfword as needed.
    const auto aligned_first = first & ~1U;
    const auto size = (count + (first - aligned_first) + 1U) & ~1U;
    if (remaining_copies == 0 || size > index_region_capacity_ - index_source_count_)
    {
        return std::nullopt;
    }
    bgfx::BufferRegion source;
    bgfx::BufferRegion destination;
    source.init(geometry.meshlet_indices(lod), aligned_first * 2U, size * 2U);
    destination.init(index_arena_, index_source_count_ * 2U, size * 2U);
    bgfx::blit(meshlet_index_upload_view, destination, source);
    const auto arena_first = index_source_count_ + (first - aligned_first);
    index_source_count_ += size;
    --remaining_copies;
    index_sources_.emplace(key, arena_first);
    return arena_first;
}

bool MeshletCullPass::dispatch(const RenderFrameData& frame, const SceneDrawView& view,
                               ViewDrawList& draws, bool sample)
{
    ENGINE_PROFILE_ZONE("Render.meshletCullSubmit");
    bool previous_result = std::exchange(result_valid_, false);
    for (auto& batch : draws.batches)
    {
        batch.meshlets = {};
    }
    if (!view.renderer || !view.world || !view.projection)
    {
        return false;
    }
    if (!bgfx::isValid(program_))
    {
        return true;
    }
    const auto fail = [&draws]
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        for (auto& batch : draws.batches)
        {
            batch.meshlets = {};
        }
        return false;
    };
    std::vector<std::uint64_t> resources;
    for (const auto& batch : draws.batches)
    {
        if (batch.items.empty())
        {
            continue;
        }
        const auto& item = draws.opaque[batch.items.front()];
        const auto identity = item.geometry->resource.handle();
        resources.insert(resources.end(),
                         {identity.owner, identity.id, identity.generation, batch.key.binding});
    }
    if (world_epoch_ != frame.world_epoch || observer_ != draws.observer ||
        resources_ != resources || sampling_ != sample)
    {
        previous_result = false;
        ++generation_;
        stats_ = {};
        world_epoch_ = frame.world_epoch;
        observer_ = draws.observer;
        stats_.world_epoch = world_epoch_;
        stats_.observer = observer_;
        resources_ = std::move(resources);
        sampling_ = sample;
    }
    stats_.state = !sample                                     ? MeshletSampleState::disabled
                   : stats_.state == MeshletSampleState::ready ? MeshletSampleState::ready
                                                               : MeshletSampleState::pending;
    Readback* readback = nullptr;
    const auto now = std::chrono::steady_clock::now();
    if (sample && now - sampled_at_ >= std::chrono::milliseconds(250))
    {
        for (auto& slot : readbacks_)
        {
            if (!slot.pending)
            {
                readback = &slot;
                break;
            }
        }
    }
    FrustumPlanes planes{};
    const bool frustum =
        view.frustum_culling_enabled &&
        build_frustum_planes(view.view, view.projection, bgfx::getCaps()->homogeneousDepth, planes);
    const bool orthographic = view.projection[15] > 0.5F;
    std::vector<std::array<float, 4>> inputs;
    using SingletonKey = std::pair<std::array<std::uint32_t, 16>, bool>;
    std::map<SingletonKey, std::uint32_t> singleton_inputs;
    const auto pack_input =
        [&](const RenderFrameData::Mesh& instance, schema::CullMode cull, bool include_normal)
    {
            ENGINE_PROFILE_FUNCTION_PROBE();
        const auto start = inputs.size();
        const auto vectors = include_normal ? 8U : 5U;
        inputs.resize(start + vectors);
        std::memcpy(inputs.data() + start, instance.model.data(), 16 * sizeof(float));
        if (include_normal)
        {
            std::memcpy(inputs.data() + start + 4, instance.normal.data(), 12 * sizeof(float));
        }
        std::array<float, 16> inverse{};
        bool cone = cone_safe(instance.model.data(), cull);
        if (cone)
        {
            bx::mtxInverse(inverse.data(), instance.model.data());
            cone = finite_matrix(inverse.data());
        }
        std::array<float, 3> position{};
        if (cone && orthographic)
        {
            for (std::size_t axis = 0; axis < 3; ++axis)
            {
                position[axis] =
                    -(inverse[axis] * view.world[8] + inverse[4 + axis] * view.world[9] +
                      inverse[8 + axis] * view.world[10]);
            }
        }
        else if (cone)
        {
            position =
                transform_point(inverse.data(), {view.world[12], view.world[13], view.world[14]});
        }
        inputs[start + vectors - 1] = {position[0], position[1], position[2], cone ? 1.0F : 0.0F};
    };
    std::uint32_t used_pairs = 0, used_commands = 0, used_clusters = 0, used_instances = 0;
    std::uint32_t singleton_commands = 0;
    [[maybe_unused]] std::uint64_t original_input_bytes = 0;
    struct Work
    {
        const GpuGeometry* geometry;
        std::size_t lod, draw_batch;
        const schema::MeshletDataT* source;
        std::uint32_t first_cluster, cluster_count, first_input, instance_count;
        std::uint32_t first_command, first_output;
        std::uint32_t clusters_per_command;
        MeshletDrawRange* output;
        std::uint32_t compact_index_count;
    };
    std::vector<Work> work;
    auto& counters = draws.opaque_stats;
    for (std::size_t draw_batch = 0; draw_batch < draws.batches.size(); ++draw_batch)
    {
        auto& batch = draws.batches[draw_batch];
        if (batch.items.empty())
        {
            continue;
        }
        const auto& item = draws.opaque[batch.items.front()];
        const auto& mesh = frame.meshes[item.mesh];
        const auto& material = mesh.gpu->materials[item.slot];
        const auto* program = material.program;
        const auto count = static_cast<std::uint32_t>(batch.items.size());
        if (!mesh.instance_eligible || mesh.plane || material.frame_textures || !program ||
            !program->definition.meshlet_culling_compatible || !program->supports_instancing() ||
            (bgfx::isValid(program->surface_program) &&
             !bgfx::isValid(program->instanced_surface_program)) ||
            item.geometry != mesh.gpu->geometry.get())
        {
            counters.meshlet_incompatible_draws += count;
            continue;
        }
        const auto* data = item.lod == 0 ? mesh.data->meshlets.get()
                                         : mesh.data->lods[item.lod - 1]->meshlets.get();
        if (!data)
        {
            counters.meshlet_missing_data += count;
            continue;
        }
        std::size_t part = 0;
        const auto ranges = item.geometry->submeshes(item.lod);
        if (!ranges.empty())
        {
            const auto found = std::find_if(
                ranges.begin(), ranges.end(), [&](const auto& range)
                {
                    ENGINE_PROFILE_FUNCTION_PROBE();
                    return range.first_index() == item.first && range.index_count() == item.count;
                });
            if (found == ranges.end())
            {
                return fail();
            }
            part = static_cast<std::size_t>(found - ranges.begin());
        }
        if (part >= data->submeshes.size())
        {
            return fail();
        }
        if (!bgfx::isValid(item.geometry->meshlet_indices(item.lod)))
        {
            // Preparation is budgeted across frames; ordinary geometry remains usable.
            continue;
        }
        const auto& range = data->submeshes[part];
        const auto pairs = static_cast<std::uint64_t>(range.meshlet_count()) * count;
        if (pairs > capacity_ - used_pairs)
        {
            counters.meshlet_budget_fallbacks += count;
            continue;
        }
        auto first_input = static_cast<std::uint32_t>(inputs.size());
        original_input_bytes += std::uint64_t{count} * 8 * sizeof(inputs[0]);
        if (count == 1)
        {
            const auto bits = std::bit_cast<std::array<std::uint32_t, 16>>(mesh.model);
            const auto [entry, inserted] = singleton_inputs.try_emplace(
                {bits, material.cull == schema::CullMode::back}, first_input);
            first_input = entry->second;
            if (inserted)
            {
                pack_input(mesh, material.cull, false);
            }
        }
        else
        {
            for (const auto index : batch.items)
            {
                pack_input(frame.meshes[draws.opaque[index].mesh], material.cull, true);
            }
        }
        const auto clusters_per_command = count == 1 ? singleton_clusters_per_command : 1U;
        const auto command_count =
            (range.meshlet_count() + clusters_per_command - 1) / clusters_per_command;
        const auto output_count = count > 1 ? static_cast<std::uint32_t>(pairs) : 0U;
        // The singleton draw retains its original base instance value.
        const auto first_output = count > 1 ? used_instances : used_pairs;
        batch.meshlets = {commands_, instances_, 0, command_count, first_output, output_count};
        const bool compact = count == 1 && command_count <= maximum_compaction_commands &&
                             program->path.starts_with("builtin://shaders/") &&
                             item.geometry->index_uses_16_bits(item.lod, true);
        work.push_back({item.geometry, item.lod, draw_batch, data, range.first_meshlet(),
                        range.meshlet_count(), first_input, count, 0, first_output,
                        clusters_per_command, &batch.meshlets, compact ? item.count : 0});
        used_pairs += static_cast<std::uint32_t>(pairs);
        used_commands += command_count;
        used_clusters += range.meshlet_count();
        used_instances += output_count;
        if (count == 1)
        {
            singleton_commands += command_count;
        }
        counters.meshlet_candidate_pairs += pairs;
        counters.meshlet_candidate_triangles += static_cast<std::uint64_t>(item.count / 3) * count;
        counters.meshlet_instanced_batches += count > 1;
    }
    ENGINE_PROFILE_PLOT("Meshlet.candidatePairs", static_cast<double>(used_pairs));
    constexpr std::uint32_t maximum_groups = 65535;
    const auto singleton_groups =
        (singleton_commands + singleton_commands_per_group - 1) / singleton_commands_per_group;
    const auto used_groups = singleton_groups + used_commands - singleton_commands;
    ENGINE_PROFILE_PLOT("Meshlet.originalInputBytes", static_cast<double>(original_input_bytes));
    ENGINE_PROFILE_PLOT("Meshlet.budgetFallbacks",
                        static_cast<double>(counters.meshlet_budget_fallbacks));
    ENGINE_PROFILE_PLOT("Meshlet.capacityPairs", static_cast<double>(capacity_));
    const auto descriptor_count = used_commands + used_clusters * 3;
    // A partial allocation failure may leave a new buffer without its previous contents.
    if (inputs.size() > input_capacity_)
    {
        uploaded_inputs_.clear();
    }
    if (descriptor_count > descriptor_capacity_)
    {
        descriptor_layout_.clear();
    }
    if (used_instances > instance_capacity_)
    {
        previous_result = false;
    }
    if (!ensure_buffer(inputs_, input_capacity_, static_cast<std::uint32_t>(inputs.size()),
                       capacity_ * 8, GpuGeometry::vector_layout(), BGFX_BUFFER_COMPUTE_READ) ||
        !ensure_buffer(instances_, instance_capacity_, used_instances, capacity_, instance_layout(),
                       BGFX_BUFFER_COMPUTE_WRITE) ||
        !ensure_buffer(descriptors_, descriptor_capacity_, descriptor_count, capacity_ * 4,
                       GpuGeometry::vector_layout(), BGFX_BUFFER_COMPUTE_READ))
    {
        return fail();
    }
    struct IndexJob
    {
        std::array<std::uint32_t, 4> args;
        std::span<const Work> batches;
    };
    std::vector<IndexJob> index_jobs;
    std::vector<std::array<std::uint32_t, 4>> index_spans;
    std::uint32_t output_indices = 0;
    [[maybe_unused]] std::uint32_t compacted_commands = 0;
    [[maybe_unused]] std::uint32_t cross_mesh_batches = 0, cross_mesh_draws_avoided = 0;
    const auto copy_limit = std::min(32U, bgfx::getCaps()->limits.maxBlits / 8U);
    auto remaining_copies = copy_limit;
    std::vector<std::uint64_t> layout;
    layout.reserve(work.size() * 10);
    std::uint32_t next_singleton = 0;
    std::uint32_t next_multi = singleton_commands;
    for (auto& batch : work)
    {
        auto& next_command = batch.instance_count == 1 ? next_singleton : next_multi;
        batch.first_command = next_command;
        batch.output->first_command = next_command;
        next_command += batch.output->command_count;
        batch.output->instances = instances_;
        const auto identity = batch.geometry->resource.handle();
        layout.insert(layout.end(),
                      {identity.owner, identity.id, identity.generation, batch.lod,
                       batch.first_cluster, batch.cluster_count, batch.first_input,
                       batch.instance_count, batch.first_command, batch.first_output});
    }
    const auto same_binding = [&](const Work& left, const Work& right)
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        const auto& left_batch = draws.batches[left.draw_batch];
        const auto& right_batch = draws.batches[right.draw_batch];
        const auto& a = left_batch.key;
        const auto& b = right_batch.key;
        if (a.state != b.state || a.program != b.program || a.binding != b.binding ||
            a.rvt_role != b.rvt_role || a.has_lightmap != b.has_lightmap ||
            a.lightmap != b.lightmap || a.has_probe != b.has_probe ||
            a.coefficients != b.coefficients)
        {
            return false;
        }
        const auto& first = draws.opaque[left_batch.items.front()];
        const auto& next = draws.opaque[right_batch.items.front()];
        const auto& first_mesh = frame.meshes[first.mesh];
        const auto& next_mesh = frame.meshes[next.mesh];
        const auto& first_material = first_mesh.gpu->materials[first.slot];
        const auto& next_material = next_mesh.gpu->materials[next.slot];
        return first_material.pbr_opaque && next_material.pbr_opaque &&
               first_material.program == next_material.program &&
               bgfx::isValid(first_material.program->depth_program) &&
               bgfx::isValid(first_material.program->surface_program) &&
               std::memcmp(first_mesh.model.data(), next_mesh.model.data(),
                           sizeof(first_mesh.model)) == 0 &&
               std::memcmp(first_mesh.normal.data(), next_mesh.normal.data(),
                           sizeof(first_mesh.normal)) == 0;
    };
    const auto add_index_job =
        [&](std::span<const Work> batches, std::uint32_t index_count, std::uint32_t commands)
    {
            ENGINE_PROFILE_FUNCTION_PROBE();
        const auto size = (index_count + 1U) & ~1U;
        const bool merged = batches.size() > 1;
        if (!size || size > index_region_capacity_ - output_indices ||
            index_jobs.size() >= index_compaction_capacity ||
            (merged && batches.size() > index_span_capacity - index_spans.size()))
        {
            return false;
        }
        if (merged)
        {
            // Reserve the whole source job before issuing any copies.
            auto space = index_region_capacity_ - index_source_count_;
            auto copies = remaining_copies;
            for (const auto& batch : batches)
            {
                const auto first = batch.source->meshlets[batch.first_cluster].first_index();
                if (!index_sources_.contains(index_source_key(*batch.geometry, batch.lod, first)))
                {
                    const auto aligned_count =
                        (batch.compact_index_count + (first & 1U) + 1U) & ~1U;
                    if (!copies || aligned_count > space)
                    {
                        return false;
                    }
                    --copies;
                    space -= aligned_count;
                }
            }
        }
        const auto first_command = batches.front().first_command;
        std::array<std::array<std::uint32_t, 4>, maximum_compaction_commands> spans;
        for (std::size_t i = 0; i < batches.size(); ++i)
        {
            const auto& batch = batches[i];
            const auto first = batch.source->meshlets[batch.first_cluster].first_index();
            const auto cached = cache_index_source(*batch.geometry, batch.lod, first,
                                                   batch.compact_index_count, remaining_copies);
            if (!cached)
            {
                return false;
            }
            spans[i] = {batch.first_command - first_command + batch.output->command_count,
                        *cached - first, merged ? batch.geometry->first_vertex() : 0U, 0};
        }
        auto source = spans[0][1];
        if (merged)
        {
            source = static_cast<std::uint32_t>(index_spans.size());
            index_spans.insert(index_spans.end(), spans.begin(), spans.begin() + batches.size());
            ++cross_mesh_batches;
            cross_mesh_draws_avoided += static_cast<std::uint32_t>(batches.size()) - 1U;
        }
        // Zero spans denotes the single-mesh fast path.
        const auto span_count = merged ? static_cast<std::uint32_t>(batches.size()) : 0U;
        index_jobs.push_back({{source, index_region_capacity_ + output_indices, first_command,
                               (span_count << 16) | commands},
                              batches});
        output_indices += size;
        compacted_commands += commands - 1U;
        return true;
    };
    struct IndexCandidate
    {
        std::size_t first, count;
        std::uint32_t indices, commands;
    };
    std::vector<IndexCandidate> candidates;
    candidates.reserve(work.size());
    std::uint64_t index_demand = 0;
    for (std::size_t first = 0; first < work.size();)
    {
        const auto& initial = work[first];
        auto end = first + 1;
        auto commands = initial.output->command_count;
        auto indices = initial.compact_index_count;
        const auto* page = initial.geometry->vertex_page();
        if (indices && page && indices <= maximum_index_region_capacity)
        {
            for (; end < work.size(); ++end)
            {
                const auto& next = work[end];
                if (next.draw_batch != work[end - 1].draw_batch + 1 ||
                    next.geometry->vertex_page() != page || !next.compact_index_count ||
                    next.output->command_count > maximum_compaction_commands - commands ||
                    next.compact_index_count > maximum_index_region_capacity - indices ||
                    !same_binding(initial, next))
                {
                    break;
                }
                commands += next.output->command_count;
                indices += next.compact_index_count;
            }
        }
        if (end > first + 1)
        {
            candidates.push_back({first, end - first, indices, commands});
            index_demand += indices + 2 * (end - first);
        }
        else if (indices && commands >= minimum_compaction_commands)
        {
            index_demand += std::uint64_t{indices} + 2;
        }
        first = end;
    }
    for (std::size_t index = 0; index < work.size(); ++index)
    {
        const auto& batch = work[index];
        if (batch.compact_index_count && batch.output->command_count >= minimum_compaction_commands)
        {
            candidates.push_back(
                {index, 1, batch.compact_index_count, batch.output->command_count});
        }
    }
    // Spend the shared index budget on the most Metal commands removed per copied index.
    // Planning order is independent of submission order.
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const auto& a, const auto& b)
                     {
                         ENGINE_PROFILE_FUNCTION_PROBE();
                         return std::uint64_t{a.commands - 1} * b.indices >
                                std::uint64_t{b.commands - 1} * a.indices;
                     });
    // Both regions start on a packed pair of 16-bit indices.
    const auto needed = static_cast<std::uint32_t>(std::min<std::uint64_t>(
        (index_demand + 1) & ~std::uint64_t{1}, maximum_index_region_capacity));
    if (needed > index_region_capacity_ && prepare_index_compaction())
    {
        const auto capacity =
            std::max(needed, std::min(maximum_index_region_capacity, index_region_capacity_ * 2));
        const auto arena =
            bgfx::createDynamicIndexBuffer(capacity * 2, BGFX_BUFFER_COMPUTE_READ_WRITE);
        if (bgfx::isValid(arena))
        {
            if (bgfx::isValid(index_arena_))
            {
                bgfx::destroy(index_arena_);
            }
            index_arena_ = arena;
            index_region_capacity_ = capacity;
            index_source_count_ = 0;
            index_sources_.clear();
            uploaded_index_jobs_.clear();
            previous_result = false;
        }
    }
    std::vector<std::uint8_t> scheduled(work.size());
    for (const auto& candidate : candidates)
    {
        auto chosen = std::span{scheduled}.subspan(candidate.first, candidate.count);
        if (std::ranges::find(chosen, 1U) == chosen.end() &&
            add_index_job(std::span{work}.subspan(candidate.first, candidate.count),
                          candidate.indices, candidate.commands))
        {
            std::ranges::fill(chosen, std::uint8_t{1});
        }
    }
    ENGINE_PROFILE_PLOT("Meshlet.crossMeshBatches", static_cast<double>(cross_mesh_batches));
    ENGINE_PROFILE_PLOT("Meshlet.crossMeshDrawsAvoided",
                        static_cast<double>(cross_mesh_draws_avoided));
    ENGINE_PROFILE_PLOT("Meshlet.indirectCommandSlots",
                        static_cast<double>(used_commands - compacted_commands));
    ENGINE_PROFILE_PLOT("Meshlet.indexCompactionBatches", static_cast<double>(index_jobs.size()));
    ENGINE_PROFILE_PLOT("Meshlet.indexSourceCopies",
                        static_cast<double>(copy_limit - remaining_copies));
    ENGINE_PROFILE_PLOT("Meshlet.indexSourceBytes", static_cast<double>(index_source_count_) * 2);
    ENGINE_PROFILE_PLOT("Meshlet.indexCompactionReservedBytes",
                        static_cast<double>(output_indices) * 2);
    const auto command_count = used_commands + static_cast<std::uint32_t>(index_jobs.size());
    std::vector<std::array<std::uint32_t, 4>> job_inputs;
    job_inputs.reserve(index_jobs.size() + index_spans.size());
    for (const auto& job : index_jobs)
    {
        auto args = job.args;
        if (job.batches.size() > 1)
        {
            args[0] += static_cast<std::uint32_t>(index_jobs.size());
        }
        job_inputs.push_back(args);
    }
    job_inputs.insert(job_inputs.end(), index_spans.begin(), index_spans.end());
    const bool upload_jobs = job_inputs != uploaded_index_jobs_;
    const bool layout_changed = layout != descriptor_layout_;
    if (command_count > command_capacity_)
    {
        previous_result = false;
        const auto count = std::max(command_count, std::min(capacity_, command_capacity_ * 2));
        const auto next = bgfx::createIndirectBuffer(count);
        if (!bgfx::isValid(next))
        {
            return fail();
        }
        bgfx::destroy(commands_);
        commands_ = next;
        command_capacity_ = count;
    }
    for (const auto& batch : work)
    {
        batch.output->commands = commands_;
    }
    if (layout_changed)
    {
        ENGINE_PROFILE_ZONE("Render.meshletPrepareDescriptors");
        // A command header points at the first of its contiguous three-vector cluster records.
        std::vector<std::array<float, 4>> descriptors(used_commands);
        descriptors.reserve(descriptor_count);
        for (const auto& batch : work)
        {
            for (std::uint32_t first = 0; first < batch.cluster_count;
                 first += batch.clusters_per_command)
            {
                const auto count =
                    std::min(batch.clusters_per_command, batch.cluster_count - first);
                const std::array<std::uint32_t, 4> command{
                    static_cast<std::uint32_t>(descriptors.size()) + first * 3, count,
                    batch.first_input, batch.instance_count};
                descriptors[batch.first_command + first / batch.clusters_per_command] =
                    std::bit_cast<std::array<float, 4>>(command);
            }
            for (std::uint32_t index = 0; index < batch.cluster_count; ++index)
            {
                const auto& cluster = batch.source->meshlets[batch.first_cluster + index];
                descriptors.push_back({static_cast<float>(cluster.center().x()),
                                       static_cast<float>(cluster.center().y()),
                                       static_cast<float>(cluster.center().z()), cluster.radius()});
                descriptors.push_back({static_cast<float>(cluster.cone_axis().x()),
                                       static_cast<float>(cluster.cone_axis().y()),
                                       static_cast<float>(cluster.cone_axis().z()),
                                       cluster.cone_cutoff()});
                descriptors.push_back(
                    {std::bit_cast<float>(cluster.first_index()),
                     std::bit_cast<float>(cluster.index_count()),
                     std::bit_cast<float>(batch.first_output + index * batch.instance_count), 0});
            }
        }
        if (!descriptors.empty())
        {
            bgfx::update(
                descriptors_, 0,
                bgfx::copy(descriptors.data(), static_cast<std::uint32_t>(descriptors.size() *
                                                                          sizeof(descriptors[0]))));
        }
        descriptor_layout_ = std::move(layout);
    }
    ENGINE_PROFILE_PLOT("Meshlet.workspaceGpuBytes",
                        static_cast<double>(input_capacity_ + descriptor_capacity_) * 16 +
                            static_cast<double>(instance_capacity_) * 112 +
                            static_cast<double>(command_capacity_) * 32 +
                            (bgfx::isValid(index_arena_)
                                 ? index_region_capacity_ * 4 +
                                       (index_compaction_capacity + index_span_capacity) * 16
                                 : 0));
    const auto input_bytes = inputs.size() * sizeof(inputs[0]);
    const bool upload_inputs =
        !inputs.empty() && (uploaded_inputs_.size() != inputs.size() ||
                            std::memcmp(uploaded_inputs_.data(), inputs.data(), input_bytes) != 0);
    const auto flags = (frustum ? 1U : 0U) | (orthographic ? 2U : 0U) | (readback ? 4U : 0U);
    const bool reuse_result =
        previous_result && !sample && !layout_changed && !upload_inputs && !upload_jobs &&
        flags == result_flags_ &&
        std::memcmp(planes.data(), result_planes_.data(), planes.size() * sizeof(planes[0])) == 0;
    ENGINE_PROFILE_PLOT("Meshlet.resultReused", static_cast<std::int64_t>(reuse_result));
    ENGINE_PROFILE_PLOT("Meshlet.indexCompactionDispatches",
                        static_cast<std::int64_t>(!reuse_result && !index_jobs.empty()));
    const auto dispatch_groups = reuse_result ? 0U : used_groups;
    ENGINE_PROFILE_PLOT("Meshlet.computeWorkgroups", static_cast<double>(dispatch_groups));
    ENGINE_PROFILE_PLOT(
        "Meshlet.computeBatches",
        static_cast<double>((dispatch_groups + maximum_groups - 1) / maximum_groups));
    ENGINE_PROFILE_PLOT("Meshlet.inputBytes", static_cast<double>(input_bytes));
    ENGINE_PROFILE_PLOT("Meshlet.inputsUploadedBytes",
                        static_cast<double>(upload_inputs ? input_bytes : 0));
    if (upload_inputs)
    {
        bgfx::update(inputs_, 0,
                     bgfx::copy(inputs.data(), static_cast<std::uint32_t>(input_bytes)));
        uploaded_inputs_ = std::move(inputs);
    }
    // A separate ordered dispatch clears global counters before any culling workgroup.
    if (readback)
    {
        bgfx::setBuffer(4, counters_, bgfx::Access::Write);
        bgfx::dispatch(meshlet_clear_view, clear_, 1);
    }
    for (std::uint32_t first = 0; first < dispatch_groups; first += maximum_groups)
    {
        const auto groups = std::min(maximum_groups, dispatch_groups - first);
        const std::array<float, 4> parameters{std::bit_cast<float>(first),
                                              std::bit_cast<float>(singleton_commands),
                                              static_cast<float>(flags), 0};
        bgfx::setUniform(parameters_, parameters.data());
        bgfx::setUniform(planes_, planes.data(), 6);
        bgfx::setBuffer(0, descriptors_, bgfx::Access::Read);
        bgfx::setBuffer(1, inputs_, bgfx::Access::Read);
        bgfx::setBuffer(2, instances_, bgfx::Access::Write);
        bgfx::setBuffer(3, commands_, bgfx::Access::Write);
        bgfx::setBuffer(4, counters_, bgfx::Access::ReadWrite);
        bgfx::dispatch(meshlet_cull_view, program_, groups);
    }
    if (!index_jobs.empty())
    {
        ENGINE_PROFILE_ZONE("Render.meshletCompactIndices");
        const auto count = static_cast<std::uint32_t>(index_jobs.size());
        const auto bytes = static_cast<std::uint32_t>(job_inputs.size() * sizeof(job_inputs[0]));
        ENGINE_PROFILE_PLOT("Meshlet.indexJobsUploadedBytes",
                            static_cast<double>(upload_jobs ? bytes : 0));
        if (upload_jobs)
        {
            uploaded_index_jobs_ = std::move(job_inputs);
            bgfx::update(index_compaction_jobs_, 0, bgfx::copy(uploaded_index_jobs_.data(), bytes));
        }
        for (std::uint32_t index = 0; index < count; ++index)
        {
            const auto& job = index_jobs[index];
            auto& output = *job.batches.front().output;
            output.first_command = used_commands + index;
            output.command_count = 1;
            output.compacted_indices = index_arena_;
            output.whole_vertex_page = job.batches.size() > 1;
            for (const auto& batch : job.batches.subspan(1))
            {
                batch.output->merged_into_previous = true;
            }
        }
        if (!reuse_result)
        {
            const float parameters[]{std::bit_cast<float>(used_commands), readback ? 1.0F : 0.0F, 0,
                                     0};
            bgfx::setUniform(index_compaction_parameters_, parameters);
            bgfx::setBuffer(0, index_compaction_jobs_, bgfx::Access::Read);
            bgfx::setBuffer(1, commands_, bgfx::Access::ReadWrite);
            bgfx::setBuffer(2, index_arena_, bgfx::Access::ReadWrite);
            bgfx::setBuffer(3, counters_, bgfx::Access::ReadWrite);
            bgfx::dispatch(meshlet_index_compact_view, index_compaction_, count);
        }
    }
    else
    {
        uploaded_index_jobs_.clear();
    }
    if (readback)
    {
        readback->stats = {};
        readback->stats.state = MeshletSampleState::pending;
        readback->stats.world_epoch = frame.world_epoch;
        readback->stats.observer = draws.observer;
        readback->stats.frame_number = view.renderer->frame_number() + 1;
        readback->stats.candidate_pairs = counters.meshlet_candidate_pairs;
        readback->stats.candidate_triangles = counters.meshlet_candidate_triangles;
        readback->counters.fill(UINT32_MAX);
        readback->generation = generation_;
        bgfx::BufferRegion region;
        region.init(counters_, 0, sizeof(readback->counters));
        readback->pending = readback->completion.submit(region, readback->counters.data());
        if (!readback->pending)
        {
            stats_.state = MeshletSampleState::failed;
        }
        sampled_at_ = now;
    }
    result_planes_ = planes;
    result_flags_ = flags;
    result_valid_ = !sample && used_groups != 0;
    return true;
}

void MeshletCullPass::poll(std::uint32_t frame_number)
{
    ENGINE_PROFILE_FUNCTION();
    for (auto& slot : readbacks_)
    {
        if (!slot.pending)
        {
            continue;
        }
        const auto result = slot.completion.status(frame_number);
        if (result == GpuReadbackStatus::pending)
        {
            continue;
        }
        slot.pending = false;
        slot.completion.reset();
        if (slot.generation != generation_)
        {
            continue;
        }
        if (stats_.state == MeshletSampleState::ready &&
            static_cast<std::int32_t>(slot.stats.frame_number - stats_.frame_number) < 0)
        {
            continue;
        }
        if (result == GpuReadbackStatus::failed)
        {
            slot.stats.state = MeshletSampleState::failed;
            stats_ = slot.stats;
            continue;
        }
        slot.stats.state =
            slot.counters[3] == 0 ? MeshletSampleState::ready : MeshletSampleState::failed;
        slot.stats.visible_pairs = slot.counters[0];
        slot.stats.nonempty_commands = slot.counters[1];
        slot.stats.visible_triangles = slot.counters[2];
        stats_ = slot.stats;
    }
}

MeshletGpuStats MeshletCullPass::statistics() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return stats_;
}

void MeshletCullPass::reset_scene() noexcept
{
    ENGINE_PROFILE_FUNCTION();
    ++generation_;
    stats_ = {};
    resources_.clear();
    world_epoch_ = 0;
    observer_ = {};
    sampling_ = false;
    release_gpu();
}

void MeshletCullPass::release_gpu() noexcept
{
    ENGINE_PROFILE_FUNCTION();
    result_valid_ = false;
    release_index_compaction();
    index_compaction_attempted_ = false;
    for (auto* handle : {&program_, &clear_})
    {
        if (bgfx::isValid(*handle))
        {
            bgfx::destroy(*handle);
            *handle = BGFX_INVALID_HANDLE;
        }
    }
    for (auto* handle : {&parameters_, &planes_})
    {
        if (bgfx::isValid(*handle))
        {
            bgfx::destroy(*handle);
            *handle = BGFX_INVALID_HANDLE;
        }
    }
    for (auto* handle : {&inputs_, &instances_, &descriptors_})
    {
        if (bgfx::isValid(*handle))
        {
            bgfx::destroy(*handle);
            *handle = BGFX_INVALID_HANDLE;
        }
    }
    if (bgfx::isValid(commands_))
    {
        bgfx::destroy(commands_);
        commands_ = BGFX_INVALID_HANDLE;
    }
    if (bgfx::isValid(counters_))
    {
        bgfx::destroy(counters_);
        counters_ = BGFX_INVALID_HANDLE;
    }
    capacity_ = 0;
    input_capacity_ = instance_capacity_ = command_capacity_ = descriptor_capacity_ = 0;
    descriptor_layout_.clear();
    uploaded_inputs_.clear();
}

void MeshletCullPass::release_index_compaction() noexcept
{
    ENGINE_PROFILE_FUNCTION();
    const auto destroy = [](auto& handle)
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        if (bgfx::isValid(handle))
        {
            bgfx::destroy(handle);
            handle = BGFX_INVALID_HANDLE;
        }
    };
    destroy(index_compaction_);
    destroy(index_compaction_parameters_);
    destroy(index_compaction_jobs_);
    destroy(index_arena_);
    index_sources_.clear();
    uploaded_index_jobs_.clear();
    index_region_capacity_ = index_source_count_ = 0;
}

std::uint32_t MeshletCullPass::resource_count() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return static_cast<std::uint32_t>(bgfx::isValid(program_)) + bgfx::isValid(clear_) +
           bgfx::isValid(parameters_) + bgfx::isValid(planes_) + bgfx::isValid(inputs_) +
           bgfx::isValid(instances_) + bgfx::isValid(commands_) + bgfx::isValid(counters_) +
           bgfx::isValid(descriptors_) + bgfx::isValid(index_compaction_) +
           bgfx::isValid(index_compaction_parameters_) + bgfx::isValid(index_compaction_jobs_) +
           bgfx::isValid(index_arena_);
}
} // namespace engine::render_detail
