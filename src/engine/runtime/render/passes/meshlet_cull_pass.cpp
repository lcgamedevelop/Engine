#include "runtime/render/passes/meshlet_cull_pass.hpp"

#include "runtime/performance/profiling.hpp"
#include "runtime/render/render_constants.hpp"
#include "runtime/render/render_math.hpp"
#include "runtime/render/render_submission.hpp"
#include "runtime/render/render_system.hpp"
#include "runtime/render/resources/gpu_program.hpp"
#include "runtime/render/scene_draw.hpp"

#include "cs_meshlet_clear.bin.h"
#include "cs_meshlet_cull.bin.h"

#include <algorithm>
#include <bit>
#include <bx/math.h>
#include <cmath>
#include <cstring>
#include <limits>

namespace engine::render_detail
{
namespace
{
bool cone_safe(const float* model, schema::CullMode cull)
{
    if (cull != schema::CullMode::back || linear_determinant(model) <= 0)
    {
        return false;
    }
    const auto dot = [model](std::size_t a, std::size_t b)
    {
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
} // namespace

bool meshlet_supported() noexcept
{
    const auto* caps = bgfx::getCaps();
    constexpr auto required = BGFX_CAPS_COMPUTE | BGFX_CAPS_DRAW_INDIRECT;
    return caps && (caps->supported & required) == required && caps->limits.maxInstanceData >= 7 &&
           caps->limits.maxComputeBindings >= 5;
}

MeshletCullPass::~MeshletCullPass() noexcept
{
    release_gpu();
}

bool MeshletCullPass::prepare(const RenderConfig& config)
{
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
    parameters_ = bgfx::createUniform("u_meshlet_parameters", bgfx::UniformType::Vec4, 3);
    planes_ = bgfx::createUniform("u_meshlet_planes", bgfx::UniformType::Vec4, 6);
    inputs_ = bgfx::createDynamicVertexBuffer(capacity * 8, GpuGeometry::vector_layout(),
                                              BGFX_BUFFER_COMPUTE_READ);
    instances_ =
        bgfx::createDynamicVertexBuffer(capacity, instance_layout(), BGFX_BUFFER_COMPUTE_WRITE);
    commands_ = bgfx::createIndirectBuffer(capacity);
    const std::array<std::uint32_t, 4> zero{};
    counters_ = bgfx::createIndexBuffer(bgfx::copy(zero.data(), sizeof(zero)),
                                        BGFX_BUFFER_INDEX32 | BGFX_BUFFER_COMPUTE_READ_WRITE);
    if (!bgfx::isValid(program_) || !bgfx::isValid(clear_) || !bgfx::isValid(parameters_) ||
        !bgfx::isValid(planes_) || !bgfx::isValid(inputs_) || !bgfx::isValid(instances_) ||
        !bgfx::isValid(commands_) || !bgfx::isValid(counters_))
    {
        release_gpu();
        return false;
    }
    capacity_ = capacity;
    bgfx::setViewName(meshlet_clear_view, "Meshlet counters");
    bgfx::setViewName(meshlet_cull_view, "Meshlet visibility");
    bgfx::setViewMode(meshlet_clear_view, bgfx::ViewMode::Sequential);
    bgfx::setViewMode(meshlet_cull_view, bgfx::ViewMode::Sequential);
    return true;
}

bool MeshletCullPass::dispatch(const RenderFrameData& frame, const SceneDrawView& view,
                               ViewDrawList& draws, bool sample)
{
    ENGINE_PROFILE_ZONE("Render.meshletCullSubmit");
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
    std::uint32_t used_pairs = 0, used_commands = 0;
    struct Work
    {
        const GpuGeometry* geometry;
        std::size_t lod;
        std::uint32_t first_cluster, cluster_count, first_input, instance_count;
        std::uint32_t first_command, first_output;
    };
    std::vector<Work> work;
    auto& counters = draws.opaque_stats;
    for (auto& batch : draws.batches)
    {
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
                { return range.first_index() == item.first && range.index_count() == item.count; });
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
        if (!bgfx::isValid(item.geometry->meshlet_indices(item.lod)) ||
            !bgfx::isValid(item.geometry->meshlet_descriptors(item.lod)))
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
        const auto first_input = static_cast<std::uint32_t>(inputs.size() / 8);
        for (const auto index : batch.items)
        {
            const auto& instance = frame.meshes[draws.opaque[index].mesh];
            const auto start = inputs.size();
            inputs.resize(start + 8);
            std::memcpy(inputs.data() + start, instance.model.data(), 16 * sizeof(float));
            std::memcpy(inputs.data() + start + 4, instance.normal.data(), 12 * sizeof(float));
            std::array<float, 16> inverse{};
            bx::mtxInverse(inverse.data(), instance.model.data());
            bool cone =
                finite_matrix(inverse.data()) && cone_safe(instance.model.data(), material.cull);
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
                position = transform_point(inverse.data(),
                                           {view.world[12], view.world[13], view.world[14]});
            }
            inputs[start + 7] = {position[0], position[1], position[2], cone ? 1.0F : 0.0F};
        }
        batch.meshlets = {commands_,     instances_,
                          used_commands, range.meshlet_count(),
                          used_pairs,    static_cast<std::uint32_t>(pairs)};
        work.push_back({item.geometry, item.lod, range.first_meshlet(), range.meshlet_count(),
                        first_input, count, used_commands, used_pairs});
        used_pairs += static_cast<std::uint32_t>(pairs);
        used_commands += range.meshlet_count();
        counters.meshlet_candidate_pairs += pairs;
        counters.meshlet_candidate_triangles += static_cast<std::uint64_t>(item.count / 3) * count;
        counters.meshlet_instanced_batches += count > 1;
    }
    if (!inputs.empty())
    {
        bgfx::update(inputs_, 0,
                     bgfx::copy(inputs.data(),
                                static_cast<std::uint32_t>(inputs.size() * sizeof(inputs[0]))));
    }
    // A separate ordered dispatch clears global counters before any culling workgroup.
    if (readback)
    {
        bgfx::setBuffer(4, counters_, bgfx::Access::Write);
        bgfx::dispatch(meshlet_clear_view, clear_, 1);
    }
    for (const auto& batch : work)
    {
        constexpr std::uint32_t maximum_groups = 65535;
        for (std::uint32_t first = 0; first < batch.cluster_count; first += maximum_groups)
        {
            const auto count = std::min(maximum_groups, batch.cluster_count - first);
            const std::array<std::uint32_t, 8> integers = {batch.first_cluster + first,
                                                           batch.first_input,
                                                           batch.first_command + first,
                                                           batch.first_output +
                                                               first * batch.instance_count,
                                                           batch.instance_count,
                                                           0,
                                                           0,
                                                           0};
            std::array<float, 12> parameters{};
            std::memcpy(parameters.data(), integers.data(), sizeof(integers));
            parameters[8] = frustum ? 1.0F : 0.0F;
            parameters[9] = orthographic ? 1.0F : 0.0F;
            parameters[10] = readback ? 1.0F : 0.0F;
            bgfx::setUniform(parameters_, parameters.data(), 3);
            bgfx::setUniform(planes_, planes.data(), 6);
            bgfx::setBuffer(0, batch.geometry->meshlet_descriptors(batch.lod), bgfx::Access::Read);
            bgfx::setBuffer(1, inputs_, bgfx::Access::Read);
            bgfx::setBuffer(2, instances_, bgfx::Access::Write);
            bgfx::setBuffer(3, commands_, bgfx::Access::Write);
            bgfx::setBuffer(4, counters_, bgfx::Access::ReadWrite);
            bgfx::dispatch(meshlet_cull_view, program_, count);
        }
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
        readback->ready_frame = bgfx::read(region, readback->counters.data());
        readback->pending = true;
        sampled_at_ = now;
    }
    return true;
}

void MeshletCullPass::poll(std::uint32_t frame_number)
{
    for (auto& slot : readbacks_)
    {
        if (!slot.pending || static_cast<std::int32_t>(frame_number - slot.ready_frame) < 0)
        {
            continue;
        }
        slot.pending = false;
        if (slot.generation != generation_)
        {
            continue;
        }
        if (stats_.state == MeshletSampleState::ready &&
            static_cast<std::int32_t>(slot.stats.frame_number - stats_.frame_number) < 0)
        {
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
    return stats_;
}

void MeshletCullPass::reset_scene() noexcept
{
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
    for (auto* handle : {&inputs_, &instances_})
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
}

std::uint32_t MeshletCullPass::resource_count() const noexcept
{
    return static_cast<std::uint32_t>(bgfx::isValid(program_)) + bgfx::isValid(clear_) +
           bgfx::isValid(parameters_) + bgfx::isValid(planes_) + bgfx::isValid(inputs_) +
           bgfx::isValid(instances_) + bgfx::isValid(commands_) + bgfx::isValid(counters_);
}
} // namespace engine::render_detail
