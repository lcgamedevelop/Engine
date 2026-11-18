#include "runtime/render/vsm/virtual_shadow_pass.hpp"

#include "cs_vsm_clear.bin.h"
#include "cs_vsm_feedback.bin.h"
#include "runtime/performance/profiling.hpp"
#include "runtime/render/passes/shadow_pass.hpp"
#include "runtime/render/render_submission.hpp"
#include "runtime/render/render_system.hpp"
#include "runtime/render/resources/gpu_program.hpp"
#include "runtime/render/scene_draw.hpp"
#include "runtime/resource/resource.hpp"

#include <algorithm>
#include <bit>
#include <span>
#include <string_view>
#include <utility>

namespace engine::render_detail
{
namespace
{
bool collect_receiver_inputs(const SceneDrawView& draw, const RenderFrameData& frame,
                             const ViewDrawList& list, bgfx::TextureHandle depth,
                             std::vector<std::uint64_t>& output)
{
    ENGINE_PROFILE_ZONE("Render.vsmReceiverInputs");
    output.clear();
    const auto append_floats = [&](std::span<const float> values)
    {
        for (const auto value : values)
        {
            output.push_back(std::bit_cast<std::uint32_t>(value));
        }
    };
    const auto append_resource = [&](ResourceHandle handle)
    {
        output.push_back(handle.owner);
        output.push_back(handle.id);
        output.push_back(handle.generation);
    };
    append_floats({draw.view, 16});
    append_floats({draw.projection, 16});
    output.insert(output.end(), {depth.idx, draw.max_anisotropy, list.opaque.size()});
    for (const auto& item : list.opaque)
    {
        const auto& mesh = frame.meshes[item.mesh];
        const auto& material = mesh.gpu->materials[item.slot];
        // Reuse only built-in depth coverage whose vertex and discard inputs are known.
        const std::string_view program = material.program->path;
        if (bgfx::isValid(mesh.gpu->deformed_vertices) || material.frame_textures ||
            (program != "builtin://shaders/pbr" && program != "builtin://shaders/terrain-pbr" &&
             program != "builtin://shaders/unlit" && program != "builtin://shaders/blinn-phong"))
        {
            return false;
        }
        append_resource(item.geometry->resource.handle());
        append_resource(material.program->resource.handle());
        output.insert(output.end(),
                      {mesh.gpu->node_id, mesh.gpu->instance_id, mesh.gpu->revision,
                       item.geometry->vertices.idx, item.geometry->indices.idx, item.slot, item.lod,
                       item.first, item.count, item.instanced_transform,
                       material.program->program.idx, static_cast<std::uint64_t>(material.cull),
                       material.values.size(), material.samplers.size(),
                       mesh.gpu->textures.size()});
        append_floats(mesh.model);
        for (const auto& value : material.values)
        {
            append_floats(value);
        }
        for (const auto& sampler : material.samplers)
        {
            output.insert(output.end(), {sampler.texture, sampler.flags});
        }
        for (const auto* texture : mesh.gpu->textures)
        {
            output.push_back(texture ? texture->generation() : 0);
        }
    }
    return true;
}
} // namespace
bool vsm_supported(const schema::VsmSettingsT& settings, std::uint32_t physical_pages) noexcept
{
    const auto* caps = bgfx::getCaps();
    const auto side = vsm_atlas_columns(physical_pages) * vsm_tile_size;
    return caps && (caps->supported & BGFX_CAPS_COMPUTE) && caps->limits.maxComputeBindings >= 6 &&
           caps->limits.maxTextureSamplers >= 16 && caps->limits.maxViews >= 256 && side &&
           side <= caps->limits.maxTextureSize && settings.virtual_resolution >= 4096 &&
           settings.virtual_resolution <= 32768 &&
           bgfx::isTextureValid(0, false, 1, bgfx::TextureFormat::D32F, shadow_atlas_flags);
}
VirtualShadowPass::~VirtualShadowPass() noexcept
{
    release_gpu();
}
bool VirtualShadowPass::prepare(const schema::VsmSettingsT& settings, const RenderConfig& config,
                                std::uint64_t generation)
{
    if (!generation || !vsm_supported(settings, config.vsm_physical_pages))
    {
        return false;
    }
    if (generation_ == generation && resolution_ == settings.virtual_resolution &&
        physical_pages_ == config.vsm_physical_pages && bgfx::isValid(framebuffer_))
    {
        return true;
    }
    release_gpu();
    generation_ = generation;
    physical_pages_ = config.vsm_physical_pages;
    resolution_ = settings.virtual_resolution;
    const auto side =
        static_cast<std::uint16_t>(vsm_atlas_columns(physical_pages_) * vsm_tile_size);
    atlas_ =
        bgfx::createTexture2D(side, side, false, 1, bgfx::TextureFormat::D32F, shadow_atlas_flags);
    const std::uint32_t white = UINT32_MAX;
    fallback_ = bgfx::createTexture2D(1, 1, false, 1, bgfx::TextureFormat::RGBA8, 0,
                                      bgfx::copy(&white, sizeof(white)));
    if (bgfx::isValid(atlas_))
    {
        framebuffer_ = bgfx::createFrameBuffer(1, &atlas_, false);
    }
    const auto pages = resolution_ / vsm_page_size;
    word_count_ = (pages * pages * vsm_max_levels + 31) / 32;
    demands_ = bgfx::createDynamicIndexBuffer(word_count_,
                                              BGFX_BUFFER_INDEX32 | BGFX_BUFFER_COMPUTE_READ_WRITE);
    bgfx::VertexLayout vertex_layout;
    vertex_layout.begin().add(bgfx::Attrib::TexCoord0, 4, bgfx::AttribType::Float).end();
    metadata_ = bgfx::createDynamicVertexBuffer(vsm_header_vectors, vertex_layout,
                                                BGFX_BUFFER_COMPUTE_READ);
    const auto make_program = [](const std::uint8_t* data, std::uint32_t size)
    {
        const auto shader = bgfx::createShader(bgfx::copy(data, size));
        return bgfx::isValid(shader) ? bgfx::createProgram(shader, true)
                                     : bgfx::ProgramHandle{bgfx::kInvalidHandle};
    };
    clear_ = make_program(cs_vsm_clear, sizeof(cs_vsm_clear));
    feedback_ = make_program(cs_vsm_feedback, sizeof(cs_vsm_feedback));
    sampler_ = bgfx::createUniform("s_virtual_shadow", bgfx::UniformType::Sampler);
    depth_sampler_ = bgfx::createUniform("s_vsm_receiver_depth", bgfx::UniformType::Sampler);
    inverse_ = bgfx::createUniform("u_vsm_inverse", bgfx::UniformType::Mat4);
    dimensions_ = bgfx::createUniform("u_vsm_feedback", bgfx::UniformType::Vec4);
    layout_ = bgfx::createUniform("u_vsm_layout", bgfx::UniformType::Vec4);
    if (!bgfx::isValid(framebuffer_) || !bgfx::isValid(fallback_) || !bgfx::isValid(demands_) ||
        !bgfx::isValid(metadata_) || !bgfx::isValid(clear_) || !bgfx::isValid(feedback_) ||
        !bgfx::isValid(sampler_) || !bgfx::isValid(depth_sampler_) || !bgfx::isValid(inverse_) ||
        !bgfx::isValid(dimensions_) || !bgfx::isValid(layout_))
    {
        release_gpu();
        return false;
    }
    bytes_ = std::uint64_t{side} * side * sizeof(float) + word_count_ * sizeof(std::uint32_t) +
             vsm_header_vectors * sizeof(float) * 4 + 4;
    return true;
}
void VirtualShadowPass::poll(std::uint32_t frame_number)
{
    ENGINE_PROFILE_ZONE("Render.vsmResolveFeedback");
    for (auto& slot : readbacks_)
    {
        if (!slot.pending || static_cast<std::int32_t>(frame_number - slot.ready_frame) < 0)
        {
            continue;
        }
        slot.pending = false;
        if (slot.view.generation != generation_)
        {
            slot.bits.clear();
            continue;
        }
        VsmPageDemand result;
        result.view = slot.view;
        result.submission_id = slot.submission_id;
        result.receiver_revision = slot.receiver_revision;
        const auto n = slot.view.pages_per_axis;
        const auto count = n * n * slot.view.level_count;
        for (std::uint32_t index = 0; index < count; ++index)
        {
            if (!(slot.bits[index / 32] & (1U << (index % 32))))
            {
                continue;
            }
            const auto level = index / (n * n);
            result.pages.push_back({static_cast<std::uint8_t>(level),
                                    slot.view.levels[level].x + index % n,
                                    slot.view.levels[level].y + (index / n) % n});
        }
        completed_.push_back(std::move(result));
        if (completed_.size() > readbacks_.size())
        {
            completed_.pop_front();
        }
    }
}
bool VirtualShadowPass::take_demand(VsmPageDemand& output)
{
    output = {};
    if (completed_.empty())
    {
        return false;
    }
    output = std::move(completed_.front());
    completed_.pop_front();
    return true;
}
bool VirtualShadowPass::submit_feedback(const VsmView& view, bgfx::TextureHandle depth,
                                        const SceneDrawView& draw, const RenderFrameData& frame,
                                        const ViewDrawList& list, std::uint64_t submission_id)
{
    if (!bgfx::isValid(depth) || !bgfx::isValid(feedback_) || view.generation != generation_)
    {
        return false;
    }
    const bool reusable = collect_receiver_inputs(draw, frame, list, depth, candidate_inputs_);
    if (reusable && (view != feedback_view_ || candidate_inputs_ != feedback_inputs_))
    {
        feedback_view_ = view;
        feedback_inputs_.swap(candidate_inputs_);
        ++receiver_revision_;
    }
    if (!reusable)
    {
        feedback_view_ = {};
        feedback_inputs_.clear();
    }
    const bool reuse =
        reusable && (accepted_receiver_revision_ == receiver_revision_ ||
                     std::any_of(readbacks_.begin(), readbacks_.end(),
                                 [&](const auto& slot)
                                 {
                                     return slot.pending &&
                                            slot.view.generation == view.generation &&
                                            slot.receiver_revision == receiver_revision_;
                                 }));
    ENGINE_PROFILE_PLOT("Vsm.feedbackReused", static_cast<std::int64_t>(reuse));
    if (reuse)
    {
        ENGINE_PROFILE_PLOT("Vsm.feedbackSkipped", std::int64_t{0});
        return true;
    }
    auto found = std::find_if(readbacks_.begin(), readbacks_.end(),
                              [](const auto& slot) { return !slot.pending; });
    if (found == readbacks_.end())
    {
        ENGINE_PROFILE_PLOT("Vsm.feedbackSkipped", std::int64_t{1});
        return true;
    }
    ENGINE_PROFILE_ZONE("Render.vsmFeedbackSubmit");
    ENGINE_PROFILE_PLOT("Vsm.feedbackSkipped", std::int64_t{0});
    auto& slot = *found;
    std::vector<std::array<float, 4>> data;
    vsm_pack_view(view, data);
    bgfx::update(
        metadata_, 0,
        bgfx::copy(data.data(), static_cast<std::uint32_t>(data.size() * sizeof(data[0]))));
    const float layout[]{0, 1, 0, -1};
    const float dimensions[]{float(view.width), float(view.height),
                             view.homogeneous_depth ? 1.F : 0.F,
                             view.origin_bottom_left ? 1.F : 0.F};
    bgfx::setViewName(vsm_feedback_clear_view, "VSM feedback clear");
    bgfx::setBuffer(4, demands_, bgfx::Access::Write);
    const float count[]{float(word_count_), 0, 0, 0};
    bgfx::setUniform(dimensions_, count);
    bgfx::dispatch(vsm_feedback_clear_view, clear_, (word_count_ + 63) / 64);
    bgfx::setViewName(vsm_feedback_view, "VSM receiver demand");
    bgfx::setUniform(layout_, layout);
    bgfx::setUniform(dimensions_, dimensions);
    bgfx::setUniform(inverse_, view.inverse_view_projection.data());
    bgfx::setTexture(0, depth_sampler_, depth);
    bgfx::setBuffer(4, demands_, bgfx::Access::ReadWrite);
    bgfx::setBuffer(5, metadata_, bgfx::Access::Read);
    // Eight threads per axis, with two receiver pixels per thread on each axis.
    bgfx::dispatch(vsm_feedback_view, feedback_, (view.width + 15) / 16, (view.height + 15) / 16);
    slot.view = view;
    slot.submission_id = submission_id;
    slot.receiver_revision = reusable ? receiver_revision_ : 0;
    slot.bits.assign(word_count_, 0);
    bgfx::BufferRegion region;
    region.init(demands_, 0, word_count_ * sizeof(std::uint32_t));
    slot.ready_frame = bgfx::read(region, slot.bits.data());
    if (slot.ready_frame == UINT32_MAX)
    {
        return false;
    }
    slot.pending = true;
    return true;
}
void VirtualShadowPass::accept_feedback(std::uint64_t receiver_revision) noexcept
{
    accepted_receiver_revision_ = receiver_revision;
}
bool VirtualShadowPass::draw_pages(const RenderFrameData& frame, const VsmPageBatch& batch,
                                   ShadowPass& shadow)
{
    ENGINE_PROFILE_ZONE("Render.vsmPageSubmit");
    if (batch.generation != generation_ || !bgfx::isValid(framebuffer_))
    {
        return false;
    }
    VsmPageTarget target;
    target.framebuffer = framebuffer_;
    target.fallback = fallback_;
    return shadow.draw_pages(frame, batch.pages, target);
}
bool VirtualShadowPass::binding(VsmGpuBinding& output) const
{
    output = {};
    if (!generation_ || !bgfx::isValid(framebuffer_))
    {
        return false;
    }
    output.atlas = atlas_;
    output.sampler = sampler_;
    output.columns = vsm_atlas_columns(physical_pages_);
    output.atlas_size = output.columns * vsm_tile_size;
    output.resident_bytes = bytes_;
    return true;
}
void VirtualShadowPass::reset_scene() noexcept
{
    release_gpu();
}
void VirtualShadowPass::release_gpu() noexcept
{
    generation_ = 0;
    receiver_revision_ = accepted_receiver_revision_ = 0;
    feedback_view_ = {};
    feedback_inputs_.clear();
    candidate_inputs_.clear();
    completed_.clear();
    if (bgfx::isValid(framebuffer_))
    {
        for (auto view : vsm_page_views)
        {
            bgfx::setViewFrameBuffer(view, BGFX_INVALID_HANDLE);
        }
    }
    const auto destroy = [](auto& handle)
    {
        if (bgfx::isValid(handle))
        {
            bgfx::destroy(handle);
        }
        handle = BGFX_INVALID_HANDLE;
    };
    destroy(framebuffer_);
    destroy(atlas_);
    destroy(fallback_);
    destroy(demands_);
    destroy(metadata_);
    destroy(clear_);
    destroy(feedback_);
    destroy(sampler_);
    destroy(depth_sampler_);
    destroy(inverse_);
    destroy(dimensions_);
    destroy(layout_);
    bytes_ = 0;
}
std::uint32_t VirtualShadowPass::resource_count() const noexcept
{
    return bgfx::isValid(atlas_) + bgfx::isValid(fallback_) + bgfx::isValid(framebuffer_) +
           bgfx::isValid(demands_) + bgfx::isValid(metadata_) + bgfx::isValid(clear_) +
           bgfx::isValid(feedback_) + bgfx::isValid(sampler_) + bgfx::isValid(depth_sampler_) +
           bgfx::isValid(inverse_) + bgfx::isValid(dimensions_) + bgfx::isValid(layout_);
}
} // namespace engine::render_detail
