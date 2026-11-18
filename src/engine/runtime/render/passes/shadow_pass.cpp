#include "runtime/render/passes/shadow_pass.hpp"
#include "runtime/render/vsm/vsm_types.hpp"

#include "runtime/performance/profiling.hpp"
#include "runtime/render/render_math.hpp"
#include "runtime/render/render_submission.hpp"
#include "runtime/render/render_system.hpp"
#include "runtime/schema/shader_generated.h"

#include "fs_shadow.bin.h"
#include "vs_shadow.bin.h"
#include "vs_shadow_instanced.bin.h"

#include <bx/math.h>

#include <SDL3/SDL_log.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <map>

namespace engine::render_detail
{
namespace
{
bool shadow_signature(const RenderFrameData& frame, std::span<const VsmPageDraw> pages,
                      const VsmPageTarget& target, std::vector<std::uint64_t>& values)
{
    ENGINE_PROFILE_ZONE("Render.shadowCacheInputs");
    values.clear();
    const auto append_floats = [&](const auto& source)
    {
        for (const float value : source)
        {
            values.push_back(std::bit_cast<std::uint32_t>(value));
        }
    };
    const auto append_resource = [&](ResourceHandle handle)
    {
        values.push_back(handle.owner);
        values.push_back(handle.id);
        values.push_back(handle.generation);
    };
    values.push_back(frame.world_epoch);
    values.push_back(target.framebuffer.idx);
    values.push_back(target.fallback.idx);
    values.push_back(target.depth_uniform.idx);
    values.push_back(target.caster_ids != nullptr);
    values.push_back(pages.size());
    for (const auto& page : pages)
    {
        values.push_back(page.view_id);
        values.push_back(page.x);
        values.push_back(page.y);
        values.push_back(page.size);
        append_floats(page.view);
        append_floats(page.projection);
    }
    values.push_back(frame.meshes.size());
    for (const auto& source : frame.meshes)
    {
        if (!source.cast_shadows || !source.gpu ||
            (target.caster_ids && std::find(target.caster_ids->begin(), target.caster_ids->end(),
                                            source.gpu->node_id) == target.caster_ids->end()))
        {
            continue;
        }
        const auto& mesh = *source.gpu;
        // GPU/CPU deformation may update buffer contents without changing its handle.
        if (bgfx::isValid(mesh.deformed_vertices) || !mesh.geometry ||
            !mesh.geometry->resource.handle().id)
        {
            return false;
        }
        values.push_back(mesh.node_id);
        values.push_back(mesh.instance_id);
        values.push_back(mesh.revision);
        append_resource(mesh.geometry->resource.handle());
        values.push_back(mesh.geometry->vertices.idx);
        values.push_back(mesh.geometry->indices.idx);
        values.push_back(source.instancing);
        values.push_back(source.instance_eligible);
        append_floats(source.model);
        append_floats(mesh.minimum);
        append_floats(mesh.maximum);
        values.push_back(mesh.materials.size());
        for (const auto& material : mesh.materials)
        {
            if (material.frame_textures ||
                (bgfx::isValid(material.shadow_texture) && !material.shadow_texture_resource.id))
            {
                return false;
            }
            values.push_back(static_cast<std::uint64_t>(material.blend));
            values.push_back(static_cast<std::uint64_t>(material.cull));
            values.push_back(material.shadow_valid);
            append_floats(material.shadow_alpha);
            append_floats(material.shadow_uv);
            values.push_back(material.shadow_texture.idx);
            append_resource(material.shadow_texture_resource);
        }
    }
    return true;
}
} // namespace

ShadowPass::~ShadowPass() noexcept
{
    release();
}

bgfx::TextureHandle ShadowPass::texture() const noexcept
{
    return color_;
}

void ShadowPass::release() noexcept
{
    if (bgfx::isValid(framebuffer_))
    {
        for (const auto view : attached_views_)
        {
            bgfx::setViewFrameBuffer(view, BGFX_INVALID_HANDLE);
        }
        bgfx::destroy(framebuffer_);
    }
    if (bgfx::isValid(color_))
    {
        bgfx::destroy(color_);
    }
    if (bgfx::isValid(depth_))
    {
        bgfx::destroy(depth_);
    }
    if (bgfx::isValid(program_))
    {
        bgfx::destroy(program_);
    }
    if (bgfx::isValid(instanced_program_))
    {
        bgfx::destroy(instanced_program_);
    }
    if (bgfx::isValid(instanced_vertex_))
    {
        bgfx::destroy(instanced_vertex_);
    }
    if (bgfx::isValid(vertex_))
    {
        bgfx::destroy(vertex_);
    }
    if (bgfx::isValid(fragment_))
    {
        bgfx::destroy(fragment_);
    }
    if (bgfx::isValid(alpha_))
    {
        bgfx::destroy(alpha_);
    }
    if (bgfx::isValid(alpha_sampler_))
    {
        bgfx::destroy(alpha_sampler_);
    }
    if (bgfx::isValid(uv_transform_))
    {
        bgfx::destroy(uv_transform_);
    }
    color_ = BGFX_INVALID_HANDLE;
    depth_ = BGFX_INVALID_HANDLE;
    framebuffer_ = BGFX_INVALID_HANDLE;
    vertex_ = BGFX_INVALID_HANDLE;
    instanced_vertex_ = BGFX_INVALID_HANDLE;
    instanced_program_ = BGFX_INVALID_HANDLE;
    fragment_ = BGFX_INVALID_HANDLE;
    program_ = BGFX_INVALID_HANDLE;
    alpha_ = BGFX_INVALID_HANDLE;
    alpha_sampler_ = BGFX_INVALID_HANDLE;
    uv_transform_ = BGFX_INVALID_HANDLE;
    resolution_ = 0;
    attached_views_.clear();
    last_skipped_lights_ = 0;
    cached_signature_.clear();
    candidate_signature_.clear();
    cache_valid_ = false;
}

bool ShadowPass::prepare(std::uint16_t size)
{
    if (resolution_ == size && bgfx::isValid(framebuffer_) && bgfx::isValid(program_))
    {
        return true;
    }
    release();
    constexpr auto color_flags = BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP |
                                 BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT |
                                 BGFX_SAMPLER_MIP_POINT;
    constexpr auto depth_flags = BGFX_TEXTURE_RT_WRITE_ONLY;
    if (size > bgfx::getCaps()->limits.maxTextureSize ||
        !bgfx::isTextureValid(0, false, 1, bgfx::TextureFormat::RGBA8, color_flags) ||
        !bgfx::isTextureValid(0, false, 1, bgfx::TextureFormat::D16, depth_flags))
    {
        return false;
    }
    color_ = bgfx::createTexture2D(size, size, false, 1, bgfx::TextureFormat::RGBA8, color_flags);
    depth_ = bgfx::createTexture2D(size, size, false, 1, bgfx::TextureFormat::D16, depth_flags);
    if (!bgfx::isValid(color_) || !bgfx::isValid(depth_))
    {
        release();
        return false;
    }
    const bgfx::TextureHandle attachments[] = {color_, depth_};
    framebuffer_ = bgfx::createFrameBuffer(2, attachments, false);
    vertex_ = bgfx::createShader(bgfx::copy(vs_shadow, sizeof(vs_shadow)));
    fragment_ = bgfx::createShader(bgfx::copy(fs_shadow, sizeof(fs_shadow)));
    if (bgfx::isValid(vertex_) && bgfx::isValid(fragment_))
    {
        program_ = bgfx::createProgram(vertex_, fragment_);
    }
    if (bgfx::getCaps()->limits.maxInstanceData >= 4)
    {
        instanced_vertex_ =
            bgfx::createShader(bgfx::copy(vs_shadow_instanced, sizeof(vs_shadow_instanced)));
        if (bgfx::isValid(instanced_vertex_) && bgfx::isValid(fragment_))
        {
            instanced_program_ = bgfx::createProgram(instanced_vertex_, fragment_);
        }
        if (!bgfx::isValid(instanced_program_))
        {
            release();
            return false;
        }
    }
    alpha_ = bgfx::createUniform("u_shadow_alpha", bgfx::UniformType::Vec4);
    alpha_sampler_ = bgfx::createUniform("s_shadow_alpha", bgfx::UniformType::Sampler);
    uv_transform_ = bgfx::createUniform("u_shadow_uv", bgfx::UniformType::Vec4);
    if (!bgfx::isValid(framebuffer_) || !bgfx::isValid(program_) || !bgfx::isValid(alpha_) ||
        !bgfx::isValid(alpha_sampler_) || !bgfx::isValid(uv_transform_))
    {
        release();
        return false;
    }
    resolution_ = size;
    return true;
}

std::uint32_t ShadowPass::resource_count() const noexcept
{
    return static_cast<std::uint32_t>(bgfx::isValid(color_)) +
           static_cast<std::uint32_t>(bgfx::isValid(depth_)) +
           static_cast<std::uint32_t>(bgfx::isValid(framebuffer_)) +
           static_cast<std::uint32_t>(bgfx::isValid(instanced_vertex_)) +
           static_cast<std::uint32_t>(bgfx::isValid(instanced_program_)) +
           static_cast<std::uint32_t>(bgfx::isValid(vertex_)) +
           static_cast<std::uint32_t>(bgfx::isValid(fragment_)) +
           static_cast<std::uint32_t>(bgfx::isValid(program_)) +
           static_cast<std::uint32_t>(bgfx::isValid(alpha_)) +
           static_cast<std::uint32_t>(bgfx::isValid(alpha_sampler_)) +
           static_cast<std::uint32_t>(bgfx::isValid(uv_transform_));
}

bool ShadowPass::bind_material(const MaterialBinding& material, bgfx::TextureHandle fallback)
{
    if (!material.shadow_valid)
    {
        return false;
    }
    bgfx::setUniform(alpha_, material.shadow_alpha.data());
    bgfx::setUniform(uv_transform_, material.shadow_uv.data());
    bgfx::setTexture(0, alpha_sampler_,
                     bgfx::isValid(material.shadow_texture) ? material.shadow_texture : fallback);
    return true;
}

bool ShadowPass::draw(const RenderFrameData& frame_data, const RenderSettings& settings,
                      ShadowFrame& frame, bgfx::TextureHandle fallback,
                      bgfx::UniformHandle depth_uniform, const RenderConfig& config,
                      std::span<const LocalShadowLight> local_lights,
                      std::span<ClusterLight> lights, const float* camera_world,
                      bgfx::ViewId base_view, const std::span<const std::uint64_t>* caster_ids)
{
    ENGINE_PROFILE_ZONE("Render.shadowSubmit");
    frame.local_records.clear();
    frame.skipped_local_lights = 0;
    for (auto& light : lights)
    {
        light.kind_inner[2] = light.kind_inner[3] = 0.0F;
    }
    const auto cascade_count = static_cast<std::uint16_t>(
        settings.directional_shadows && frame.tile_size != 0 ? settings.shadow_cascades : 0);
    const auto maximum_views =
        static_cast<std::uint32_t>(cascade_count) + config.maximum_local_shadow_faces;
    if (!bgfx::isValid(framebuffer_) || resolution_ != config.shadow_atlas_size ||
        (cascade_count != 0 && settings.shadow_resolution > resolution_) ||
        config.local_shadow_tile_size == 0 || config.local_shadow_tile_size > resolution_ ||
        camera_world == nullptr || !finite_matrix(camera_world) ||
        std::uint32_t{base_view} + maximum_views > bgfx::getCaps()->limits.maxViews)
    {
        return false;
    }
    std::vector<VsmPageDraw> pages;
    pages.reserve(maximum_views);
    struct Caster
    {
        const GpuMesh* mesh = nullptr;
        const RenderFrameData::Mesh* source = nullptr;
        std::array<float, 16> model{};
        std::array<std::array<float, 3>, 8> world_points{};
        ShadowBounds bounds;
    };
    std::vector<Caster> casters;
    casters.reserve(frame_data.meshes.size());
    for (const auto& source : frame_data.meshes)
    {
        const auto& mesh = *source.gpu;
        if (caster_ids != nullptr &&
            std::find(caster_ids->begin(), caster_ids->end(), mesh.node_id) == caster_ids->end())
        {
            continue;
        }
        if (!source.cast_shadows)
        {
            continue;
        }
        Caster caster;
        caster.mesh = &mesh;
        caster.source = &source;
        caster.model = source.model;
        caster.bounds.reset();
        float combined[16]{};
        bx::mtxMul(combined, caster.model.data(), frame.view.data());
        for (std::uint16_t corner = 0; corner < 8; ++corner)
        {
            const std::array<float, 3> point = {
                (corner & 1) != 0 ? mesh.maximum[0] : mesh.minimum[0],
                (corner & 2) != 0 ? mesh.maximum[1] : mesh.minimum[1],
                (corner & 4) != 0 ? mesh.maximum[2] : mesh.minimum[2]};
            caster.bounds.include(transform_point(combined, point));
            caster.world_points[corner] = transform_point(caster.model.data(), point);
        }
        casters.push_back(caster);
    }
    const auto* caps = bgfx::getCaps();
    const float crop[] = {0.5F,
                          0.0F,
                          0.0F,
                          0.0F,
                          0.0F,
                          caps->originBottomLeft ? 0.5F : -0.5F,
                          0.0F,
                          0.0F,
                          0.0F,
                          0.0F,
                          caps->homogeneousDepth ? 0.5F : 1.0F,
                          0.0F,
                          0.5F,
                          0.5F,
                          caps->homogeneousDepth ? 0.5F : 0.0F,
                          1.0F};
    for (std::uint16_t index = 0; index < cascade_count; ++index)
    {
        auto bounds = frame.bounds[index];
        for (const auto& caster : casters)
        {
            if (bounds.overlaps_xy(caster.bounds))
            {
                // Include off-screen occluders whose light-space footprint reaches the
                // receiver slice. Camera-frustum culling alone would lose their shadows.
                bounds.minimum[2] = std::min(bounds.minimum[2], caster.bounds.minimum[2]);
                bounds.maximum[2] = std::max(bounds.maximum[2], caster.bounds.maximum[2]);
            }
        }
        const float margin = std::max(0.1F, (bounds.maximum[2] - bounds.minimum[2]) * 0.01F);
        bounds.minimum[2] -= margin;
        bounds.maximum[2] += margin;
        float projection[16]{};
        bx::mtxOrtho(projection, bounds.minimum[0], bounds.maximum[0], bounds.minimum[1],
                     bounds.maximum[1], -bounds.maximum[2], -bounds.minimum[2], 0.0F,
                     caps->homogeneousDepth, bx::Handedness::Right);
        float projected[16]{};
        bx::mtxMul(projected, projection, crop);
        bx::mtxMul(frame.matrices[index].data(), frame.view.data(), projected);
        if (!finite_matrix(frame.matrices[index].data()))
        {
            return false;
        }
        const auto view = static_cast<std::uint16_t>(base_view + index);
        const auto tile_x = static_cast<std::uint16_t>((index % 2) * frame.tile_size);
        const auto tile_y = static_cast<std::uint16_t>((index / 2) * frame.tile_size);
        const float scale = static_cast<float>(frame.tile_size) / resolution_;
        frame.rectangles[index] = {static_cast<float>(tile_x) / resolution_,
                                   caps->originBottomLeft
                                       ? 1.0F - static_cast<float>(tile_y) / resolution_ - scale
                                       : static_cast<float>(tile_y) / resolution_,
                                   scale, scale};
        VsmPageDraw page;
        page.view_id = view;
        page.x = tile_x;
        page.y = tile_y;
        page.size = frame.tile_size;
        page.view = frame.view;
        std::copy_n(projection, 16, page.projection.begin());
        pages.push_back(page);
    }
    std::vector<const LocalShadowLight*> candidates;
    for (const auto& light : local_lights)
    {
        if (!light.settings.cast_shadows || light.settings.kind == schema::LightKind::directional)
        {
            continue;
        }
        if (light.light_index >= lights.size() || !finite_matrix(light.world.data()))
        {
            return false;
        }
        candidates.push_back(&light);
    }
    const auto distance_squared = [camera_world](const LocalShadowLight& light)
    {
        double distance = 0.0;
        for (std::size_t axis = 0; axis < 3; ++axis)
        {
            const double offset =
                static_cast<double>(light.world[12 + axis]) - camera_world[12 + axis];
            distance += offset * offset;
        }
        return distance;
    };
    std::sort(candidates.begin(), candidates.end(),
              [&](const auto* left, const auto* right)
              {
                  const auto a = distance_squared(*left);
                  const auto b = distance_squared(*right);
                  return a != b ? a < b : left->node_id < right->node_id;
              });
    // Fixed-size cells deliberately avoid a second atlas allocator. Smaller requested
    // faces occupy the upper-left of their cell; CSM keeps its original complete region.
    std::vector<std::array<std::uint16_t, 2>> free_tiles;
    const auto cell_size = config.local_shadow_tile_size;
    const auto cells_per_row = static_cast<std::uint16_t>(resolution_ / cell_size);
    for (std::uint16_t y = 0; y < cells_per_row; ++y)
    {
        for (std::uint16_t x = 0; x < cells_per_row; ++x)
        {
            const auto px = static_cast<std::uint16_t>(x * cell_size);
            const auto py = static_cast<std::uint16_t>(y * cell_size);
            if (cascade_count != 0 && px < settings.shadow_resolution &&
                py < settings.shadow_resolution)
            {
                continue;
            }
            free_tiles.push_back({px, py});
        }
    }
    const std::array<bx::Vec3, 6> point_directions = {
        bx::Vec3{1.0F, 0.0F, 0.0F},  bx::Vec3{-1.0F, 0.0F, 0.0F}, bx::Vec3{0.0F, 1.0F, 0.0F},
        bx::Vec3{0.0F, -1.0F, 0.0F}, bx::Vec3{0.0F, 0.0F, 1.0F},  bx::Vec3{0.0F, 0.0F, -1.0F}};
    const std::array<bx::Vec3, 6> point_ups = {
        bx::Vec3{0.0F, -1.0F, 0.0F}, bx::Vec3{0.0F, -1.0F, 0.0F}, bx::Vec3{0.0F, 0.0F, 1.0F},
        bx::Vec3{0.0F, 0.0F, -1.0F}, bx::Vec3{0.0F, -1.0F, 0.0F}, bx::Vec3{0.0F, -1.0F, 0.0F}};
    for (const auto* light : candidates)
    {
        const bool point = light->settings.kind == schema::LightKind::point;
        const std::uint16_t faces = point ? 6 : 1;
        const auto first_record = frame.local_records.size();
        if (first_record + faces > config.maximum_local_shadow_faces ||
            first_record + faces > free_tiles.size())
        {
            ++frame.skipped_local_lights;
            continue;
        }
        const auto size = std::min(light->settings.shadow_resolution, cell_size);
        const bx::Vec3 position{light->world[12], light->world[13], light->world[14]};
        const float near_plane = light->settings.range * 0.001F;
        const float far_plane = light->settings.range;
        if (!(near_plane > 0.0F && far_plane > near_plane))
        {
            return false;
        }
        for (std::uint16_t face = 0; face < faces; ++face)
        {
            bx::Vec3 direction = point_directions[face];
            bx::Vec3 up = point_ups[face];
            if (!point)
            {
                const auto& value = lights[light->light_index].direction_outer;
                direction = {value[0], value[1], value[2]};
                up = std::abs(direction.y) > 0.95F ? bx::Vec3{0.0F, 0.0F, 1.0F}
                                                   : bx::Vec3{0.0F, 1.0F, 0.0F};
            }
            float view_matrix[16]{};
            float projection[16]{};
            bx::mtxLookAt(view_matrix, position, bx::add(position, direction), up,
                          bx::Handedness::Right);
            bx::mtxProj(projection, point ? 90.0F : light->settings.outer_angle * 2.0F, 1.0F,
                        near_plane, far_plane, caps->homogeneousDepth, bx::Handedness::Right);
            ShadowRecord record;
            float projected[16]{};
            bx::mtxMul(projected, projection, crop);
            bx::mtxMul(record.matrix.data(), view_matrix, projected);
            if (!finite_matrix(record.matrix.data()))
            {
                return false;
            }
            const auto tile = free_tiles[first_record + face];
            const float scale = static_cast<float>(size) / resolution_;
            record.rectangle = {static_cast<float>(tile[0]) / resolution_,
                                caps->originBottomLeft
                                    ? 1.0F - static_cast<float>(tile[1]) / resolution_ - scale
                                    : static_cast<float>(tile[1]) / resolution_,
                                scale, scale};
            record.parameters = {light->settings.shadow_bias, 1.0F / resolution_, near_plane,
                                 far_plane};
            const auto view =
                static_cast<bgfx::ViewId>(base_view + cascade_count + first_record + face);
            VsmPageDraw page;
            page.view_id = view;
            page.x = tile[0];
            page.y = tile[1];
            page.size = size;
            std::copy_n(view_matrix, 16, page.view.begin());
            std::copy_n(projection, 16, page.projection.begin());
            pages.push_back(page);
            frame.local_records.push_back(record);
        }
        lights[light->light_index].kind_inner[2] = static_cast<float>(first_record + 1);
        lights[light->light_index].kind_inner[3] = static_cast<float>(faces);
    }
    if (frame.skipped_local_lights != last_skipped_lights_)
    {
        if (frame.skipped_local_lights != 0)
        {
            SDL_LogWarn(SDL_LOG_CATEGORY_RENDER,
                        "Shadow atlas budget: %u local lights have no shadow allocation",
                        frame.skipped_local_lights);
        }
        last_skipped_lights_ = frame.skipped_local_lights;
    }
    if (pages.empty())
    {
        return true;
    }
    VsmPageTarget target{framebuffer_, fallback, depth_uniform, nullptr};
    std::vector<std::uint64_t> selected;
    if (caster_ids)
    {
        selected.assign(caster_ids->begin(), caster_ids->end());
        target.caster_ids = &selected;
    }
    const bool cacheable = shadow_signature(frame_data, pages, target, candidate_signature_);
    if (cacheable && cache_valid_ && cached_signature_ == candidate_signature_)
    {
        ENGINE_PROFILE_PLOT("Render.shadowCacheHit", std::int64_t{1});
        ENGINE_PROFILE_PLOT("Render.shadowSubmittedPages", std::int64_t{0});
        return true;
    }
    ENGINE_PROFILE_PLOT("Render.shadowCacheHit", std::int64_t{0});
    ENGINE_PROFILE_PLOT("Render.shadowSubmittedPages", static_cast<std::int64_t>(pages.size()));
    cache_valid_ = false;
    // Prepare caster world bounds once for the whole atlas, preserving page/view order.
    if (!draw_pages(frame_data, pages, target))
    {
        return false;
    }
    if (cacheable)
    {
        cached_signature_.swap(candidate_signature_);
        cache_valid_ = true;
    }
    return true;
}

bool ShadowPass::draw_pages(const RenderFrameData& frame_data, std::span<const VsmPageDraw> pages,
                            const VsmPageTarget& target)
{
    if (target.framebuffer.idx == framebuffer_.idx)
    {
        // Direct callers may overwrite this atlas; only draw() publishes a complete key.
        cache_valid_ = false;
    }
    if (!bgfx::isValid(target.framebuffer) || !bgfx::isValid(program_) ||
        !bgfx::isValid(target.depth_uniform) || !bgfx::isValid(target.fallback))
    {
        return false;
    }
    const auto* caps = bgfx::getCaps();
    const float depth_scale[]{caps->homogeneousDepth ? .5F : 1.F,
                              caps->homogeneousDepth ? .5F : 0.F, 0, 0};
    struct Caster
    {
        const GpuMesh* mesh = nullptr;
        const RenderFrameData::Mesh* source = nullptr;
        std::array<float, 16> model{};
        std::array<std::array<float, 3>, 8> world_points{};
    };
    std::vector<Caster> casters;
    for (const auto& source : frame_data.meshes)
    {
        if (!source.cast_shadows || !source.gpu ||
            (target.caster_ids && std::find(target.caster_ids->begin(), target.caster_ids->end(),
                                            source.gpu->node_id) == target.caster_ids->end()))
        {
            continue;
        }
        Caster caster;
        caster.mesh = source.gpu;
        caster.source = &source;
        caster.model = source.model;
        for (unsigned corner = 0; corner < 8; ++corner)
        {
            std::array<float, 3> point{};
            for (unsigned axis = 0; axis < 3; ++axis)
            {
                point[axis] =
                    corner & (1U << axis) ? source.gpu->maximum[axis] : source.gpu->minimum[axis];
            }
            caster.world_points[corner] = transform_point(source.model.data(), point);
        }
        casters.push_back(caster);
    }
    struct Item
    {
        const Caster* caster = nullptr;
        const MaterialBinding* material = nullptr;
    };
    std::vector<Item> items;
    std::vector<DrawBatch> batches;
    std::map<DrawBatchKey, std::size_t> batch_index;
    const auto collect_caster = [&](const Caster& caster)
    {
        const auto& mesh = *caster.mesh;
        const auto* data = caster.source->data;
        const auto collect = [&](std::uint32_t first, std::uint32_t count, std::size_t slot)
        {
            if (slot >= mesh.materials.size())
            {
                return false;
            }
            const auto& material = mesh.materials[slot];
            if (material.blend != schema::BlendMode::opaque)
            {
                return true;
            }
            if (!material.shadow_valid)
            {
                return false;
            }
            DrawBatchKey key;
            key.geometry = reinterpret_cast<std::uintptr_t>(mesh.geometry.get());
            key.program = instanced_program_.idx;
            key.state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z |
                        BGFX_STATE_DEPTH_TEST_LESS | cull_state(material.cull, caster.model.data());
            key.first = first;
            key.count = count;
            key.shadow_alpha = material.shadow_alpha;
            key.shadow_uv = material.shadow_uv;
            key.shadow_texture = material.shadow_texture.idx;
            const auto item = items.size();
            items.push_back({&caster, &material});
            if (!target.caster_ids && caster.source->instancing &&
                caster.source->instance_eligible && bgfx::isValid(instanced_program_))
            {
                auto [found, inserted] = batch_index.try_emplace(key, batches.size());
                if (inserted)
                {
                    batches.push_back({key, {}, 0, {}});
                }
                batches[found->second].items.push_back(item);
            }
            else
            {
                batches.push_back({key, {item}, 0, {}});
            }
            return true;
        };
        if (data->submeshes.empty() &&
            !collect(0, static_cast<std::uint32_t>(data->indices.size()), 0))
        {
            return false;
        }
        for (const auto& submesh : data->submeshes)
        {
            if (!collect(submesh.first_index(), submesh.index_count(), submesh.material_slot()))
            {
                return false;
            }
        }
        return true;
    };
    const auto submit_batches = [&](bgfx::ViewId view)
    {
        for (auto& batch : batches)
        {
            while (batch.cursor < batch.items.size())
            {
                const auto& item = items[batch.items[batch.cursor]];
                const auto& caster = *item.caster;
                if (!bind_material(*item.material, target.fallback))
                {
                    return false;
                }
                constexpr std::uint16_t stride = 16 * sizeof(float);
                const auto remaining =
                    static_cast<std::uint32_t>(batch.items.size() - batch.cursor);
                const auto available =
                    remaining > 1 ? bgfx::getAvailInstanceDataBuffer(remaining, stride) : 0;
                const auto count = available >= 2 ? available : 1U;
                if (count > 1)
                {
                    bgfx::InstanceDataBuffer buffer;
                    bgfx::allocInstanceDataBuffer(&buffer, count, stride);
                    for (std::uint32_t index = 0; index < count; ++index)
                    {
                        const auto* instance = items[batch.items[batch.cursor + index]].caster;
                        std::memcpy(buffer.data + index * stride, instance->model.data(), stride);
                    }
                    bgfx::setInstanceDataBuffer(&buffer);
                }
                batch.cursor += count;
                bgfx::setTransform(caster.model.data());
                bgfx::setUniform(target.depth_uniform, depth_scale);
                caster.mesh->bind_vertices();
                bgfx::setIndexBuffer(caster.mesh->geometry->indices, batch.key.first,
                                     batch.key.count);
                bgfx::setState(batch.key.state);
                bgfx::submit(view, count > 1 ? instanced_program_ : program_);
            }
        }
        items.clear();
        batches.clear();
        batch_index.clear();
        return true;
    };

    for (const auto& page : pages)
    {
        if (!page.size || page.view_id >= caps->limits.maxViews ||
            !finite_matrix(page.view.data()) || !finite_matrix(page.projection.data()))
        {
            return false;
        }
        bgfx::setViewName(page.view_id, "Shadow page");
        bgfx::setViewFrameBuffer(page.view_id, target.framebuffer);
        bgfx::setViewRect(page.view_id, page.x, page.y, page.size, page.size);
        bgfx::setViewClear(page.view_id, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0xffffffff, 1.F, 0);
        bgfx::setViewTransform(page.view_id, page.view.data(), page.projection.data());
        bgfx::touch(page.view_id);
        if (target.framebuffer.idx == framebuffer_.idx &&
            std::find(attached_views_.begin(), attached_views_.end(), page.view_id) ==
                attached_views_.end())
        {
            attached_views_.push_back(page.view_id);
        }
        float view_projection[16];
        bx::mtxMul(view_projection, page.view.data(), page.projection.data());
        for (const auto& caster : casters)
        {
            std::uint32_t outside = 63;
            for (const auto& corner : caster.world_points)
            {
                float clip[4]{};
                for (unsigned row = 0; row < 4; ++row)
                {
                    clip[row] = view_projection[row] * corner[0] +
                                view_projection[4 + row] * corner[1] +
                                view_projection[8 + row] * corner[2] + view_projection[12 + row];
                }
                const std::uint32_t mask =
                    (clip[0] < -clip[3] ? 1U : 0U) | (clip[0] > clip[3] ? 2U : 0U) |
                    (clip[1] < -clip[3] ? 4U : 0U) | (clip[1] > clip[3] ? 8U : 0U) |
                    (clip[2] < (caps->homogeneousDepth ? -clip[3] : 0.F) ? 16U : 0U) |
                    (clip[2] > clip[3] ? 32U : 0U);
                outside &= mask;
            }
            if (!outside && !collect_caster(caster))
            {
                return false;
            }
        }
        if (!submit_batches(page.view_id))
        {
            return false;
        }
    }
    return true;
}

} // namespace engine::render_detail
