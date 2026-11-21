#include "runtime/render/passes/surface_pass.hpp"

#include "runtime/performance/function_profile.hpp"
#include "runtime/performance/profiling.hpp"
#include "runtime/render/render_constants.hpp"
#include "runtime/render/render_submission.hpp"
#include "runtime/render/resources/gpu_program.hpp"
#include "runtime/render/scene_draw.hpp"
#include "runtime/resource/resource.hpp"

#include <algorithm>
#include <bit>

namespace engine::render_detail
{

SurfacePass::~SurfacePass() noexcept
{
    ENGINE_PROFILE_FUNCTION();
    release();
}

bool SurfacePass::prepare(std::uint16_t width, std::uint16_t height, bool material_surface)
{
    ENGINE_PROFILE_FUNCTION();
    if (bgfx::isValid(framebuffer_) && width == width_ && height == height_ &&
        material_surface == material_surface_)
    {
        return true;
    }
    release();
    const auto* caps = bgfx::getCaps();
    constexpr auto flags =
        BGFX_TEXTURE_RT | BGFX_SAMPLER_UVW_CLAMP | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT;
    if (width == 0 || height == 0 || caps->limits.maxFBAttachments < (material_surface ? 3 : 1) ||
        width > caps->limits.maxTextureSize || height > caps->limits.maxTextureSize ||
        (material_surface &&
         !bgfx::isTextureValid(0, false, 1, bgfx::TextureFormat::RGBA16F, flags)) ||
        !bgfx::isTextureValid(0, false, 1, bgfx::TextureFormat::D32F, flags))
    {
        return false;
    }
    if (material_surface)
    {
        targets_[0] =
            bgfx::createTexture2D(width, height, false, 1, bgfx::TextureFormat::RGBA16F, flags);
        targets_[1] =
            bgfx::createTexture2D(width, height, false, 1, bgfx::TextureFormat::RGBA16F, flags);
    }
    targets_[2] = bgfx::createTexture2D(width, height, false, 1, bgfx::TextureFormat::D32F, flags);
    const auto first = material_surface ? targets_.begin() : targets_.begin() + 2;
    if (!std::all_of(first, targets_.end(), [](auto h)
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        return bgfx::isValid(h);
    }))
    {
        release();
        return false;
    }
    framebuffer_ = bgfx::createFrameBuffer(
        material_surface ? 3 : 1, material_surface ? targets_.data() : targets_.data() + 2, false);
    if (!bgfx::isValid(framebuffer_))
    {
        release();
        return false;
    }
    width_ = width;
    height_ = height;
    material_surface_ = material_surface;
    return true;
}

bool SurfacePass::can_combine_depth(const RenderFrameData& frame, const ViewDrawList& list) const
{
    ENGINE_PROFILE_FUNCTION();
    return material_surface_ && can_submit_opaque_depth(frame, list) &&
           std::all_of(list.opaque.begin(), list.opaque.end(),
                       [&](const auto& item)
                       {
                           ENGINE_PROFILE_FUNCTION_PROBE();
                           const auto& mesh = frame.meshes[item.mesh];
                           const auto* program = mesh.gpu->materials[item.slot].program;
                           return bgfx::isValid(program->surface_program);
                       });
}

void SurfacePass::begin(const float* view, const float* projection, bool combined_depth)
{
    ENGINE_PROFILE_FUNCTION();
    for (auto id : {ambient_occlusion_depth_view, surface_view})
    {
        if ((id == surface_view && !material_surface_) ||
            (id == ambient_occlusion_depth_view && combined_depth))
        {
            continue;
        }
        const auto* name = combined_depth        ? "Scene depth and material surface"
                           : id == surface_view ? "Material surface"
                                                : "Scene depth";
        bgfx::setViewName(id, name);
        bgfx::setViewFrameBuffer(id, framebuffer_);
        bgfx::setViewRect(id, 0, 0, width_, height_);
        bgfx::setViewTransform(id, view, projection);
        bgfx::setViewMode(id, bgfx::ViewMode::Sequential);
        const bool clear = combined_depth || id == ambient_occlusion_depth_view;
        const auto flags = BGFX_CLEAR_DEPTH | (material_surface_ ? BGFX_CLEAR_COLOR : 0);
        bgfx::setViewClear(id, clear ? flags : BGFX_CLEAR_NONE, 0, 1.0F, 0);
        bgfx::touch(id);
    }
}

bgfx::TextureHandle SurfacePass::depth() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return targets_[2];
}

std::uint64_t SurfacePass::identify_depth(const SceneDrawView& draw, const RenderFrameData& frame,
                                          const ViewDrawList& list)
{
    ENGINE_PROFILE_ZONE("Render.sceneDepthInputs");
    auto& inputs = candidate_inputs_;
    inputs.clear();
    const auto append_floats = [&](std::span<const float> values)
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        for (const auto value : values)
        {
            inputs.push_back(std::bit_cast<std::uint32_t>(value));
        }
    };
    const auto append_resource = [&](ResourceHandle handle)
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        inputs.insert(inputs.end(), {handle.owner, handle.id, handle.generation});
    };
    append_floats({draw.view, 16});
    append_floats({draw.projection, 16});
    inputs.insert(inputs.end(),
                  {frame.world_epoch, depth().idx, width_, height_, draw.max_anisotropy,
                   draw.frustum_culling_enabled, list.opaque.size(), list.batches.size()});
    for (const auto& batch : list.batches)
    {
        inputs.insert(inputs.end(), {bgfx::isValid(batch.meshlets.commands), batch.items.size()});
        inputs.insert(inputs.end(), batch.items.begin(), batch.items.end());
    }
    for (const auto& item : list.opaque)
    {
        const auto& mesh = frame.meshes[item.mesh];
        const auto& material = mesh.gpu->materials[item.slot];
        // A specialized depth program certifies the coverage inputs for these built-in materials.
        if (bgfx::isValid(mesh.gpu->deformed_vertices) || material.frame_textures ||
            !bgfx::isValid(material.program->depth_program))
        {
            depth_inputs_.clear();
            return 0;
        }
        append_resource(item.geometry->resource.handle());
        append_resource(material.program->resource.handle());
        inputs.insert(inputs.end(),
                      {mesh.gpu->node_id, mesh.gpu->instance_id, mesh.gpu->revision,
                       item.geometry->position_buffer_index(), item.geometry->first_vertex(),
                       item.geometry->index_buffer(item.lod).idx,
                       item.slot, item.lod,
                       item.first, item.count, item.instanced_transform,
                       material.program->depth_program.idx,
                       static_cast<std::uint64_t>(material.cull), material.values.size(),
                       material.samplers.size(), mesh.gpu->textures.size()});
        append_floats(mesh.model);
        for (const auto& value : material.values)
        {
            append_floats(value);
        }
        for (const auto& sampler : material.samplers)
        {
            inputs.insert(inputs.end(), {sampler.texture, sampler.flags});
        }
        for (const auto* texture : mesh.gpu->textures)
        {
            inputs.push_back(texture ? texture->generation() : 0);
        }
    }
    if (inputs != depth_inputs_)
    {
        depth_inputs_.swap(inputs);
        ++depth_revision_;
    }
    return depth_revision_;
}

bgfx::TextureHandle SurfacePass::normal_roughness() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return targets_[0];
}

bgfx::TextureHandle SurfacePass::specular() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return targets_[1];
}

std::uint32_t SurfacePass::resource_count() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return static_cast<std::uint32_t>(bgfx::isValid(framebuffer_)) +
           static_cast<std::uint32_t>(std::count_if(targets_.begin(), targets_.end(),
                                                    [](auto h)
                                                    {
                                                        ENGINE_PROFILE_FUNCTION_PROBE();
                                                        return bgfx::isValid(h);
                                                    }));
}

void SurfacePass::release() noexcept
{
    ENGINE_PROFILE_FUNCTION();
    depth_inputs_.clear();
    candidate_inputs_.clear();
    if (bgfx::isValid(framebuffer_))
    {
        for (auto view : {ambient_occlusion_depth_view, surface_view})
        {
            bgfx::setViewFrameBuffer(view, BGFX_INVALID_HANDLE);
        }
        bgfx::destroy(framebuffer_);
        framebuffer_ = BGFX_INVALID_HANDLE;
    }
    for (auto& target : targets_)
    {
        if (bgfx::isValid(target))
        {
            bgfx::destroy(target);
            target = BGFX_INVALID_HANDLE;
        }
    }
    width_ = height_ = 0;
    material_surface_ = false;
}

} // namespace engine::render_detail
