#include "runtime/render/vsm/virtual_shadow_pass.hpp"

#include "cs_vsm_clear.bin.h"
#include "cs_vsm_depth_ranges.bin.h"
#include "cs_vsm_feedback.bin.h"
#include "runtime/performance/function_profile.hpp"
#include "runtime/performance/profiling.hpp"
#include "runtime/render/passes/shadow_pass.hpp"
#include "runtime/render/render_system.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace engine::render_detail
{
namespace
{
constexpr std::uint32_t local_range_columns = 6;
constexpr std::uint32_t local_range_row_bits = 12;
} // namespace

bool vsm_supported(const schema::VsmSettingsT& settings, std::uint32_t physical_pages) noexcept
{
    ENGINE_PROFILE_FUNCTION();
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
    ENGINE_PROFILE_FUNCTION();
    release_gpu();
}
bool VirtualShadowPass::prepare(const schema::VsmSettingsT& settings, const RenderConfig& config,
                                std::uint64_t generation)
{
    ENGINE_PROFILE_FUNCTION();
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
    const auto make_program = [](const std::uint8_t* data, std::uint32_t size)
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
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
    view_uniform_ = bgfx::createUniform("u_vsm_view", bgfx::UniformFreq::View,
                                        bgfx::UniformType::Vec4, vsm_view_vectors);
    if (!bgfx::isValid(framebuffer_) || !bgfx::isValid(fallback_) || !bgfx::isValid(demands_) ||
        !bgfx::isValid(clear_) || !bgfx::isValid(feedback_) || !bgfx::isValid(sampler_) ||
        !bgfx::isValid(depth_sampler_) || !bgfx::isValid(inverse_) || !bgfx::isValid(dimensions_) ||
        !bgfx::isValid(view_uniform_))
    {
        release_gpu();
        return false;
    }
    bytes_ = std::uint64_t{side} * side * sizeof(float) + word_count_ * sizeof(std::uint32_t) + 4;
    const auto* caps = bgfx::getCaps();
    // Match the Metal-only RG32F storage image in cs_vsm_depth_ranges.
    const bool compact_ranges = caps->rendererType == bgfx::RendererType::Metal;
    const auto range_format =
        compact_ranges ? bgfx::TextureFormat::RG32F : bgfx::TextureFormat::RGBA32F;
    constexpr auto range_flags = BGFX_TEXTURE_COMPUTE_WRITE | BGFX_SAMPLER_UVW_CLAMP |
                                 BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT;
    // Metal supports separate RG32Float writes and reads. bgfx's image caps
    // report read_write access, which this write-only compute kernel does not use.
    const auto range_access =
        compact_ranges ? BGFX_CAPS_FORMAT_TEXTURE_2D : BGFX_CAPS_FORMAT_TEXTURE_IMAGE_WRITE;
    if ((caps->formats[range_format] & range_access) &&
        bgfx::isTextureValid(0, false, 1, range_format, range_flags))
    {
        const auto range_side = side / vsm_depth_range_block_size;
        const auto stride = config.local_shadow_tile_size / vsm_depth_range_block_size;
        const auto faces = config.maximum_local_shadow_faces;
        const auto rows = (faces + local_range_columns - 1) / local_range_columns;
        auto width = range_side;
        auto height = range_side;
        if (faces != 0 && stride != 0 && range_side < (1U << local_range_row_bits) &&
            stride < (1U << local_range_row_bits) &&
            local_range_columns * stride <= caps->limits.maxTextureSize &&
            range_side + rows * stride <= caps->limits.maxTextureSize)
        {
            local_range_first_row_ = static_cast<std::uint16_t>(range_side);
            local_range_stride_ = static_cast<std::uint16_t>(stride);
            local_range_faces_ = faces;
            width = std::max(width, local_range_columns * stride);
            height += rows * stride;
        }
        depth_ranges_ = bgfx::createTexture2D(static_cast<std::uint16_t>(width),
                                             static_cast<std::uint16_t>(height), false, 1,
                                             range_format, range_flags);
        reduce_depth_ranges_ = make_program(cs_vsm_depth_ranges, sizeof(cs_vsm_depth_ranges));
        range_tile_origins_ =
            bgfx::createUniform("u_vsm_range_tile_origins", bgfx::UniformType::Vec4,
                                static_cast<std::uint16_t>(vsm_page_views.size()));
        range_output_ = bgfx::createUniform("u_vsm_range_output", bgfx::UniformType::Vec4);
        if (bgfx::isValid(depth_ranges_) && bgfx::isValid(reduce_depth_ranges_) &&
            bgfx::isValid(range_tile_origins_) && bgfx::isValid(range_output_))
        {
            bytes_ += std::uint64_t{width} * height * (compact_ranges ? 2 : 4) * sizeof(float);
        }
        else
        {
            release_depth_ranges();
        }
    }
    return true;
}
void VirtualShadowPass::poll(std::uint32_t frame_number)
{
    ENGINE_PROFILE_ZONE("Render.vsmResolveFeedback");
    for (auto& slot : readbacks_)
    {
        if (!slot.pending)
        {
            continue;
        }
        const auto result_status = slot.completion.status(frame_number);
        if (result_status == GpuReadbackStatus::pending)
        {
            continue;
        }
        slot.pending = false;
        slot.completion.reset();
        if (result_status != GpuReadbackStatus::complete || slot.view.generation != generation_)
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
    ENGINE_PROFILE_FUNCTION();
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
                                        std::span<const std::uint64_t> depth_inputs,
                                        std::uint64_t submission_id)
{
    if (!bgfx::isValid(depth) || !bgfx::isValid(feedback_) || view.generation != generation_)
    {
        return false;
    }
    const bool reusable = !depth_inputs.empty();
    if (reusable && (view != feedback_view_ || !std::ranges::equal(depth_inputs, feedback_inputs_)))
    {
        feedback_view_ = view;
        feedback_inputs_.assign(depth_inputs.begin(), depth_inputs.end());
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
                                     ENGINE_PROFILE_FUNCTION_PROBE();
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
                              [](const auto& slot)
                              {
                                  ENGINE_PROFILE_FUNCTION_PROBE();
                                  return !slot.pending;
                              });
    if (found == readbacks_.end())
    {
        ENGINE_PROFILE_PLOT("Vsm.feedbackSkipped", std::int64_t{1});
        return true;
    }
    ENGINE_PROFILE_ZONE("Render.vsmFeedbackSubmit");
    ENGINE_PROFILE_PLOT("Vsm.feedbackSkipped", std::int64_t{0});
    auto& slot = *found;
    const auto view_uniforms = vsm_pack_view(view);
    const float dimensions[]{float(view.width), float(view.height),
                             view.homogeneous_depth ? 1.F : 0.F,
                             view.origin_bottom_left ? 1.F : 0.F};
    bgfx::setViewName(vsm_feedback_clear_view, "VSM feedback clear");
    bgfx::setBuffer(4, demands_, bgfx::Access::Write);
    const float count[]{float(word_count_), 0, 0, 0};
    bgfx::setUniform(dimensions_, count);
    bgfx::dispatch(vsm_feedback_clear_view, clear_, (word_count_ + 63) / 64);
    bgfx::setViewName(vsm_feedback_view, "VSM receiver demand");
    bgfx::setViewUniform(vsm_feedback_view, view_uniform_, view_uniforms.data(), vsm_view_vectors);
    bgfx::setUniform(dimensions_, dimensions);
    bgfx::setUniform(inverse_, view.inverse_view_projection.data());
    bgfx::setTexture(0, depth_sampler_, depth);
    bgfx::setBuffer(4, demands_, bgfx::Access::ReadWrite);
    // Eight threads per axis, with two receiver pixels per thread on each axis.
    bgfx::dispatch(vsm_feedback_view, feedback_, (view.width + 15) / 16, (view.height + 15) / 16);
    slot.view = view;
    slot.submission_id = submission_id;
    slot.receiver_revision = reusable ? receiver_revision_ : 0;
    slot.bits.assign(word_count_, 0);
    bgfx::BufferRegion region;
    region.init(demands_, 0, word_count_ * sizeof(std::uint32_t));
    slot.pending = slot.completion.submit(region, slot.bits.data());
    return slot.pending;
}
void VirtualShadowPass::accept_feedback(std::uint64_t receiver_revision) noexcept
{
    ENGINE_PROFILE_FUNCTION();
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
    if (!shadow.draw_pages(frame, batch.pages, target))
    {
        return false;
    }
    const auto pages = bgfx::isValid(depth_ranges_) ? batch.pages.size() : 0;
    ENGINE_PROFILE_PLOT("Vsm.depthRangesUpdatedPages", static_cast<std::int64_t>(pages));
    if (pages == 0)
    {
        return true;
    }

    {
        ENGINE_PROFILE_ZONE("Render.vsmDepthRanges");
        const auto side = vsm_atlas_columns(physical_pages_) * vsm_tile_size;
        std::array<std::array<float, 4>, vsm_page_views.size()> tile_origins{};
        for (std::size_t i = 0; i < pages; ++i)
        {
            const auto& page = batch.pages[i];
            const auto y = batch.view.origin_bottom_left ? side - page.y - vsm_tile_size : page.y;
            tile_origins[i] = {float(page.x), float(y), float(page.x + vsm_tile_size - 1),
                               float(y + vsm_tile_size - 1)};
        }
        constexpr auto groups =
            static_cast<std::uint16_t>(vsm_tile_size / vsm_depth_range_block_size);
        const auto page_count = static_cast<std::uint16_t>(pages);
        constexpr float output[4]{};
        bgfx::setViewName(vsm_depth_ranges_view, "VSM page depth ranges");
        bgfx::setUniform(range_tile_origins_, tile_origins.data(), page_count);
        bgfx::setUniform(range_output_, output);
        bgfx::setTexture(0, sampler_, atlas_);
        bgfx::setImage(1, depth_ranges_, 0, bgfx::Access::Write);
        bgfx::dispatch(vsm_depth_ranges_view, reduce_depth_ranges_, groups, groups, page_count);
    }
    return true;
}
float VirtualShadowPass::submit_local_depth_ranges(bgfx::TextureHandle atlas,
                                                  std::span<const ShadowRecord> records,
                                                  std::uint64_t depth_revision)
{
    if (!local_range_stride_ || !bgfx::isValid(atlas) || records.empty() ||
        records.size() > local_range_faces_)
    {
        return 0;
    }

    ENGINE_PROFILE_ZONE("Render.localShadowDepthRanges");
    std::vector<std::array<float, 4>> tiles;
    tiles.reserve(records.size());
    for (const auto& record : records)
    {
        const auto& rectangle = record.rectangle;
        const auto texel = record.parameters[1];
        const auto x = std::round(rectangle[0] / texel);
        const auto y = std::round(rectangle[1] / texel);
        const auto width = std::round(rectangle[2] / texel);
        const auto height = std::round(rectangle[3] / texel);
        const auto extent = local_range_stride_ * vsm_depth_range_block_size;
        if (!(width >= 1 && height >= 1 && width <= extent && height <= extent))
        {
            return 0;
        }
        tiles.push_back({x, y, x + width - 1, y + height - 1});
    }

    const auto layout = (std::uint32_t{local_range_stride_} << local_range_row_bits) |
                        local_range_first_row_;
    if (atlas.idx == local_range_atlas_.idx && depth_revision == local_range_revision_ &&
        tiles == local_range_tiles_)
    {
        ENGINE_PROFILE_PLOT("Render.localShadowDepthRangeFaces", std::int64_t{0});
        return static_cast<float>(layout);
    }

    ENGINE_PROFILE_PLOT("Render.localShadowDepthRangeFaces",
                        static_cast<std::int64_t>(tiles.size()));
    bgfx::setViewName(local_shadow_depth_ranges_view, "Local shadow depth ranges");
    for (std::size_t first = 0; first < tiles.size(); first += vsm_page_views.size())
    {
        const auto count = static_cast<std::uint16_t>(
            std::min(vsm_page_views.size(), tiles.size() - first));
        const float output[]{float(local_range_first_row_), float(local_range_stride_),
                             static_cast<float>(first), 0};
        bgfx::setUniform(range_tile_origins_, tiles.data() + first, count);
        bgfx::setUniform(range_output_, output);
        bgfx::setTexture(0, sampler_, atlas);
        bgfx::setImage(1, depth_ranges_, 0, bgfx::Access::Write);
        bgfx::dispatch(local_shadow_depth_ranges_view, reduce_depth_ranges_, local_range_stride_,
                       local_range_stride_, count);
    }
    local_range_atlas_ = atlas;
    local_range_revision_ = depth_revision;
    local_range_tiles_ = std::move(tiles);
    return static_cast<float>(layout);
}

bool VirtualShadowPass::binding(VsmGpuBinding& output) const
{
    ENGINE_PROFILE_FUNCTION();
    output = {};
    if (!generation_ || !bgfx::isValid(framebuffer_))
    {
        return false;
    }
    output.atlas = atlas_;
    output.depth_ranges = depth_ranges_;
    output.sampler = sampler_;
    output.columns = vsm_atlas_columns(physical_pages_);
    output.atlas_size = output.columns * vsm_tile_size;
    output.resident_bytes = bytes_;
    return true;
}
void VirtualShadowPass::reset_scene() noexcept
{
    ENGINE_PROFILE_FUNCTION();
    release_gpu();
}
void VirtualShadowPass::release_depth_ranges() noexcept
{
    ENGINE_PROFILE_FUNCTION();
    if (bgfx::isValid(depth_ranges_))
    {
        bgfx::destroy(depth_ranges_);
        depth_ranges_ = BGFX_INVALID_HANDLE;
    }
    if (bgfx::isValid(reduce_depth_ranges_))
    {
        bgfx::destroy(reduce_depth_ranges_);
        reduce_depth_ranges_ = BGFX_INVALID_HANDLE;
    }
    if (bgfx::isValid(range_tile_origins_))
    {
        bgfx::destroy(range_tile_origins_);
        range_tile_origins_ = BGFX_INVALID_HANDLE;
    }
    if (bgfx::isValid(range_output_))
    {
        bgfx::destroy(range_output_);
        range_output_ = BGFX_INVALID_HANDLE;
    }
    local_range_atlas_ = BGFX_INVALID_HANDLE;
    local_range_revision_ = 0;
    local_range_first_row_ = local_range_stride_ = local_range_faces_ = 0;
    local_range_tiles_.clear();
}
void VirtualShadowPass::release_gpu() noexcept
{
    ENGINE_PROFILE_FUNCTION();
    generation_ = 0;
    receiver_revision_ = accepted_receiver_revision_ = 0;
    feedback_view_ = {};
    feedback_inputs_.clear();
    completed_.clear();
    release_depth_ranges();
    if (bgfx::isValid(framebuffer_))
    {
        for (auto view : vsm_page_views)
        {
            bgfx::setViewFrameBuffer(view, BGFX_INVALID_HANDLE);
        }
    }
    const auto destroy = [](auto& handle)
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
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
    destroy(clear_);
    destroy(feedback_);
    destroy(sampler_);
    destroy(depth_sampler_);
    destroy(inverse_);
    destroy(dimensions_);
    destroy(view_uniform_);
    bytes_ = 0;
}
std::uint32_t VirtualShadowPass::resource_count() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return bgfx::isValid(atlas_) + bgfx::isValid(fallback_) + bgfx::isValid(framebuffer_) +
           bgfx::isValid(demands_) + bgfx::isValid(clear_) + bgfx::isValid(feedback_) +
           bgfx::isValid(sampler_) + bgfx::isValid(depth_sampler_) + bgfx::isValid(inverse_) +
           bgfx::isValid(dimensions_) + bgfx::isValid(view_uniform_) +
           bgfx::isValid(depth_ranges_) + bgfx::isValid(reduce_depth_ranges_) +
           bgfx::isValid(range_tile_origins_) + bgfx::isValid(range_output_);
}
} // namespace engine::render_detail
