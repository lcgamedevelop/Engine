#include "runtime/render/passes/shadow_pass.hpp"
#include "runtime/render/vsm/virtual_shadow_cache.hpp"
#include "runtime/render/vsm/vsm_types.hpp"

#include "runtime/performance/function_profile.hpp"
#include "runtime/performance/profiling.hpp"
#include "runtime/render/render_math.hpp"
#include "runtime/render/render_submission.hpp"
#include "runtime/render/render_system.hpp"
#include "runtime/render/resources/gpu_program.hpp"
#include "runtime/resource/mesh_storage.hpp"
#include "runtime/schema/shader_generated.h"

#include "fs_shadow.bin.h"
#include "fs_shadow_solid.bin.h"
#include "vs_shadow.bin.h"
#include "vs_shadow_instanced.bin.h"
#include "vs_shadow_position.bin.h"

#include <bx/math.h>

#include <SDL3/SDL_log.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <map>
#include <utility>

namespace engine::render_detail
{
namespace
{
bool shadow_caster_signature(const RenderFrameData& frame, const VsmPageTarget& target,
                             std::vector<std::uint64_t>& values)
{
    ENGINE_PROFILE_ZONE("Render.shadowCacheInputs");
    values.clear();
    const auto append_floats = [&](const auto& source)
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        for (const float value : source)
        {
            values.push_back(std::bit_cast<std::uint32_t>(value));
        }
    };
    const auto append_resource = [&](ResourceHandle handle)
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        values.push_back(handle.owner);
        values.push_back(handle.id);
        values.push_back(handle.generation);
    };
    values.push_back(frame.world_epoch);
    values.push_back(target.framebuffer.idx);
    values.push_back(target.fallback.idx);
    values.push_back(target.caster_ids != nullptr);
    values.push_back(frame.meshes.size());
    for (std::size_t index = 0; index < frame.meshes.size(); ++index)
    {
        const auto& source = frame.meshes[index];
        if (!source.cast_shadows || !source.any_opaque || !source.gpu ||
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
        values.push_back(index);
        values.push_back(mesh.node_id);
        values.push_back(mesh.instance_id);
        values.push_back(mesh.revision);
        append_resource(mesh.geometry->resource.handle());
        values.push_back(mesh.geometry->position_buffer_index());
        values.push_back(mesh.geometry->first_vertex());
        values.push_back(mesh.geometry->index_buffer().idx);
        values.push_back(source.instancing);
        values.push_back(source.instance_eligible);
        if (target.caster_ids)
        {
            append_floats(source.model);
        }
        else
        {
            for (const double value : source.world)
            {
                values.push_back(std::bit_cast<std::uint64_t>(value));
            }
        }
        append_floats(mesh.minimum);
        append_floats(mesh.maximum);
        values.push_back(mesh.materials.size());
        for (const auto& material : mesh.materials)
        {
            values.push_back(static_cast<std::uint64_t>(material.blend));
            if (material.blend != schema::BlendMode::opaque)
            {
                continue;
            }
            if (material.frame_textures ||
                (bgfx::isValid(material.shadow_texture) && !material.shadow_texture_resource.id))
            {
                return false;
            }
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

bool overlaps(const auto& a, const auto& b)
{
    ENGINE_PROFILE_FUNCTION();
    return a.x < b.x + b.size && b.x < a.x + a.size && a.y < b.y + b.size && b.y < a.y + a.size;
}
} // namespace

ShadowPass::~ShadowPass() noexcept
{
    ENGINE_PROFILE_FUNCTION();
    release();
}

bgfx::TextureHandle ShadowPass::texture() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return depth_;
}

std::uint64_t ShadowPass::local_depth_revision() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return local_depth_revision_;
}

void ShadowPass::release() noexcept
{
    ENGINE_PROFILE_FUNCTION();
    ++local_depth_revision_;
    release_solid_batches();
    if (bgfx::isValid(framebuffer_))
    {
        for (const auto view : attached_views_)
        {
            bgfx::setViewFrameBuffer(view, BGFX_INVALID_HANDLE);
        }
        bgfx::destroy(framebuffer_);
    }
    if (bgfx::isValid(depth_))
    {
        bgfx::destroy(depth_);
    }
    for (auto* program : {&program_, &instanced_program_, &solid_program_,
                          &instanced_solid_program_, &solid_batch_program_})
    {
        if (bgfx::isValid(*program))
        {
            bgfx::destroy(*program);
            *program = BGFX_INVALID_HANDLE;
        }
    }
    for (auto* shader : {&vertex_, &instanced_vertex_, &fragment_, &solid_fragment_})
    {
        if (bgfx::isValid(*shader))
        {
            bgfx::destroy(*shader);
            *shader = BGFX_INVALID_HANDLE;
        }
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
    depth_ = BGFX_INVALID_HANDLE;
    framebuffer_ = BGFX_INVALID_HANDLE;
    alpha_ = BGFX_INVALID_HANDLE;
    alpha_sampler_ = BGFX_INVALID_HANDLE;
    uv_transform_ = BGFX_INVALID_HANDLE;
    resolution_ = 0;
    attached_views_.clear();
    last_skipped_lights_ = 0;
    cached_pages_.clear();
    caster_signature_.clear();
    candidate_signature_.clear();
}

bool ShadowPass::prepare(std::uint16_t size)
{
    ENGINE_PROFILE_FUNCTION();
    if (resolution_ == size && bgfx::isValid(framebuffer_) && bgfx::isValid(program_))
    {
        return true;
    }
    release();
    if (size > bgfx::getCaps()->limits.maxTextureSize ||
        !bgfx::isTextureValid(0, false, 1, bgfx::TextureFormat::D32F, shadow_atlas_flags))
    {
        return false;
    }
    depth_ =
        bgfx::createTexture2D(size, size, false, 1, bgfx::TextureFormat::D32F, shadow_atlas_flags);
    if (!bgfx::isValid(depth_))
    {
        release();
        return false;
    }
    framebuffer_ = bgfx::createFrameBuffer(1, &depth_, false);
    vertex_ = bgfx::createShader(bgfx::copy(vs_shadow, sizeof(vs_shadow)));
    fragment_ = bgfx::createShader(bgfx::copy(fs_shadow, sizeof(fs_shadow)));
    solid_fragment_ = bgfx::createShader(bgfx::copy(fs_shadow_solid, sizeof(fs_shadow_solid)));
    if (!bgfx::isValid(vertex_) || !bgfx::isValid(fragment_) || !bgfx::isValid(solid_fragment_))
    {
        release();
        return false;
    }
    program_ = bgfx::createProgram(vertex_, fragment_);
    solid_program_ = bgfx::createProgram(vertex_, solid_fragment_);
    const auto solid_batch_vertex =
        bgfx::createShader(bgfx::copy(vs_shadow_position, sizeof(vs_shadow_position)));
    if (bgfx::isValid(solid_batch_vertex))
    {
        solid_batch_program_ = bgfx::createProgram(solid_batch_vertex, solid_fragment_);
        bgfx::destroy(solid_batch_vertex);
    }
    if (bgfx::getCaps()->limits.maxInstanceData >= 4)
    {
        instanced_vertex_ =
            bgfx::createShader(bgfx::copy(vs_shadow_instanced, sizeof(vs_shadow_instanced)));
        if (bgfx::isValid(instanced_vertex_))
        {
            instanced_program_ = bgfx::createProgram(instanced_vertex_, fragment_);
            instanced_solid_program_ = bgfx::createProgram(instanced_vertex_, solid_fragment_);
        }
        if (!bgfx::isValid(instanced_program_) || !bgfx::isValid(instanced_solid_program_))
        {
            release();
            return false;
        }
    }
    alpha_ = bgfx::createUniform("u_shadow_alpha", bgfx::UniformType::Vec4);
    alpha_sampler_ = bgfx::createUniform("s_shadow_alpha", bgfx::UniformType::Sampler);
    uv_transform_ = bgfx::createUniform("u_shadow_uv", bgfx::UniformType::Vec4);
    if (!bgfx::isValid(framebuffer_) || !bgfx::isValid(program_) ||
        !bgfx::isValid(solid_program_) || !bgfx::isValid(alpha_) ||
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
    ENGINE_PROFILE_FUNCTION();
    return static_cast<std::uint32_t>(bgfx::isValid(depth_)) +
           static_cast<std::uint32_t>(bgfx::isValid(framebuffer_)) +
           static_cast<std::uint32_t>(bgfx::isValid(instanced_vertex_)) +
           static_cast<std::uint32_t>(bgfx::isValid(instanced_program_)) +
           static_cast<std::uint32_t>(bgfx::isValid(instanced_solid_program_)) +
           static_cast<std::uint32_t>(bgfx::isValid(vertex_)) +
           static_cast<std::uint32_t>(bgfx::isValid(fragment_)) +
           static_cast<std::uint32_t>(bgfx::isValid(solid_fragment_)) +
           static_cast<std::uint32_t>(bgfx::isValid(program_)) +
           static_cast<std::uint32_t>(bgfx::isValid(solid_program_)) +
           static_cast<std::uint32_t>(bgfx::isValid(solid_batch_program_)) +
           static_cast<std::uint32_t>(2 * solid_batches_.size()) +
           static_cast<std::uint32_t>(bgfx::isValid(alpha_)) +
           static_cast<std::uint32_t>(bgfx::isValid(alpha_sampler_)) +
           static_cast<std::uint32_t>(bgfx::isValid(uv_transform_));
}

bool ShadowPass::bind_material(const MaterialBinding& material, bgfx::TextureHandle fallback)
{
    ENGINE_PROFILE_FUNCTION();
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
                      ShadowFrame& frame, bgfx::TextureHandle fallback, const RenderConfig& config,
                      std::span<const LocalShadowLight> local_lights,
                      std::span<ClusterLight> lights, const float* camera_world,
                      bgfx::ViewId base_view, ShadowAtlasView atlas_view, std::size_t observer,
                      const std::span<const std::uint64_t>* caster_ids)
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
        observer >= observer_tiles_.size() || maximum_views > tile_capacity ||
        atlas_view.index >= atlas_view.count ||
        std::uint32_t{base_view} + maximum_views > bgfx::getCaps()->limits.maxViews)
    {
        return false;
    }
    std::vector<VsmPageDraw> pages;
    pages.reserve(maximum_views);
    const std::uint32_t cell_size = config.local_shadow_tile_size;
    const std::uint32_t cells_per_row = resolution_ / cell_size;
    const std::uint32_t atlas_cells = cells_per_row * cells_per_row;
    const std::uint32_t directional_cells =
        cascade_count == 0 ? 0 : (settings.shadow_resolution + cell_size - 1) / cell_size;
    const std::uint32_t reserved_cells = directional_cells * directional_cells;
    std::uint32_t local_faces = 0;
    for (const auto& light : local_lights)
    {
        if (light.settings.cast_shadows && light.settings.kind != schema::LightKind::directional)
        {
            local_faces += light.settings.kind == schema::LightKind::point ? 6U : 1U;
        }
    }
    local_faces = std::min({local_faces, std::uint32_t{config.maximum_local_shadow_faces},
                            atlas_cells - reserved_cells});
    const auto regions_per_row = directional_cells ? cells_per_row / directional_cells : 1U;
    // Reserve the same per-view face budget before choosing disjoint regions.
    // Small atlases retain the original ordered writes to the shared region.
    if ((directional_cells && atlas_view.count > regions_per_row * regions_per_row) ||
        atlas_view.count * (reserved_cells + local_faces) > atlas_cells)
    {
        atlas_view = {};
    }
    const auto directional_x = (atlas_view.index % regions_per_row) * directional_cells * cell_size;
    const auto directional_y = (atlas_view.index / regions_per_row) * directional_cells * cell_size;
    const bool limit_receivers =
        cascade_count && !caster_ids && frame.receiver_bounds_available &&
        std::ranges::all_of(frame_data.meshes,
                            [](const auto& source)
                            {
                                ENGINE_PROFILE_FUNCTION_PROBE();
                                return !source.gpu ||
                                       std::ranges::all_of(
                                           source.gpu->materials,
                                           [](const auto& material)
                                           {
                                               ENGINE_PROFILE_FUNCTION_PROBE();
                                               if (!material.program)
                                               {
                                                   return false;
                                               }
                                               const auto& path = material.program->path;
                                               return path == "builtin://shaders/pbr" ||
                                                      path == "builtin://shaders/unlit" ||
                                                      path == "builtin://shaders/blinn-phong" ||
                                                      path == "builtin://shaders/terrain-pbr" ||
                                                      path == "builtin://shaders/water";
                                           });
                            });
    std::vector<ShadowBounds> caster_bounds;
    if (cascade_count != 0)
    {
        caster_bounds.reserve(frame_data.meshes.size());
        for (const auto& source : frame_data.meshes)
        {
            if (!source.cast_shadows || !source.gpu)
            {
                continue;
            }
            const auto& mesh = *source.gpu;
            if (caster_ids != nullptr && std::find(caster_ids->begin(), caster_ids->end(),
                                                   mesh.node_id) == caster_ids->end())
            {
                continue;
            }
            auto& bounds = caster_bounds.emplace_back();
            bounds.reset();
            float combined[16]{};
            bx::mtxMul(combined, source.model.data(), frame.view.data());
            for (std::uint16_t corner = 0; corner < 8; ++corner)
            {
                const std::array<float, 3> point = {
                    (corner & 1) != 0 ? mesh.maximum[0] : mesh.minimum[0],
                    (corner & 2) != 0 ? mesh.maximum[1] : mesh.minimum[1],
                    (corner & 4) != 0 ? mesh.maximum[2] : mesh.minimum[2]};
                bounds.include(transform_point(combined, point));
            }
        }
    }
    const auto* caps = bgfx::getCaps();
    const auto atlas_rectangle = [this, caps](std::uint16_t x, std::uint16_t y, std::uint16_t size)
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        const float scale = static_cast<float>(size) / resolution_;
        const float left = static_cast<float>(x) / resolution_;
        const float top = static_cast<float>(y) / resolution_;
        return std::array{left, caps->originBottomLeft ? 1.0F - top - scale : top, scale, scale};
    };
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
        for (const auto& caster : caster_bounds)
        {
            if (bounds.overlaps_xy(caster))
            {
                // Include off-screen occluders whose light-space footprint reaches the
                // receiver slice. Camera-frustum culling alone would lose their shadows.
                bounds.minimum[2] = std::min(bounds.minimum[2], caster.minimum[2]);
                bounds.maximum[2] = std::max(bounds.maximum[2], caster.maximum[2]);
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
        const auto tile_x =
            static_cast<std::uint16_t>(directional_x + (index % 2) * frame.tile_size);
        const auto tile_y =
            static_cast<std::uint16_t>(directional_y + (index / 2) * frame.tile_size);
        frame.rectangles[index] = atlas_rectangle(tile_x, tile_y, frame.tile_size);
        VsmPageDraw page;
        page.view_id = view;
        page.x = tile_x;
        page.y = tile_y;
        page.size = frame.tile_size;
        page.view = frame.view;
        std::copy_n(projection, 16, page.projection.begin());
        if (limit_receivers)
        {
            // The PCF taps extend by at most 2.5 texels. Leave additional room for
            // raster and floating-point rounding around the receiver slice.
            constexpr float filter_guard_texels = 4.0F;
            const auto& receivers = frame.receiver_bounds[index];
            const float guard_x =
                (bounds.maximum[0] - bounds.minimum[0]) * filter_guard_texels / page.size;
            const float guard_y =
                (bounds.maximum[1] - bounds.minimum[1]) * filter_guard_texels / page.size;
            page.caster_clip_xy = {
                std::max(-1.0F, (receivers.minimum[0] - guard_x) * projection[0] + projection[12]),
                std::max(-1.0F, (receivers.minimum[1] - guard_y) * projection[5] + projection[13]),
                std::min(1.0F, (receivers.maximum[0] + guard_x) * projection[0] + projection[12]),
                std::min(1.0F, (receivers.maximum[1] + guard_y) * projection[5] + projection[13])};
        }
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
        ENGINE_PROFILE_FUNCTION_PROBE();
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
                  ENGINE_PROFILE_FUNCTION_PROBE();
                  const auto a = distance_squared(*left);
                  const auto b = distance_squared(*right);
                  return a != b ? a < b : left->node_id < right->node_id;
              });
    // Smaller faces occupy the upper-left of a fixed cell. Every view gets its own
    // contiguous slice of the cells outside the reserved directional regions.
    std::vector<std::array<std::uint16_t, 2>> free_tiles;
    free_tiles.reserve(local_faces);
    const auto first_local = atlas_view.index * local_faces;
    std::uint32_t free_index = 0;
    for (std::uint32_t y = 0; y < cells_per_row && free_tiles.size() < local_faces; ++y)
    {
        for (std::uint32_t x = 0; x < cells_per_row && free_tiles.size() < local_faces; ++x)
        {
            if (directional_cells &&
                y / directional_cells * regions_per_row + x / directional_cells < atlas_view.count)
            {
                continue;
            }
            if (free_index++ >= first_local)
            {
                free_tiles.push_back({static_cast<std::uint16_t>(x * cell_size),
                                      static_cast<std::uint16_t>(y * cell_size)});
            }
        }
    }
    const std::array<bx::Vec3, 6> point_directions = {
        bx::Vec3{1.0F, 0.0F, 0.0F},  bx::Vec3{-1.0F, 0.0F, 0.0F}, bx::Vec3{0.0F, 1.0F, 0.0F},
        bx::Vec3{0.0F, -1.0F, 0.0F}, bx::Vec3{0.0F, 0.0F, 1.0F},  bx::Vec3{0.0F, 0.0F, -1.0F}};
    const std::array<bx::Vec3, 6> point_ups = {
        bx::Vec3{0.0F, -1.0F, 0.0F}, bx::Vec3{0.0F, -1.0F, 0.0F}, bx::Vec3{0.0F, 0.0F, 1.0F},
        bx::Vec3{0.0F, 0.0F, -1.0F}, bx::Vec3{0.0F, -1.0F, 0.0F}, bx::Vec3{0.0F, -1.0F, 0.0F}};
    std::vector<LocalProjection> local_projections;
    local_projections.reserve(local_faces);
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
        const auto size =
            std::min(light->settings.shadow_resolution, config.local_shadow_tile_size);
        const bx::Vec3 position{light->world[12], light->world[13], light->world[14]};
        const float near_plane = light->settings.range * 0.001F;
        const float far_plane = light->settings.range;
        if (!(near_plane > 0.0F && far_plane > near_plane))
        {
            return false;
        }
        const float depth_scale = point ? far_plane / (far_plane - near_plane) : 0.0F;
        const float depth_numerator = depth_scale * near_plane;
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
            record.rectangle = atlas_rectangle(tile[0], tile[1], size);
            record.parameters = {light->settings.shadow_bias, 1.0F / resolution_, depth_scale,
                                 depth_numerator};
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
            if (!caster_ids)
            {
                local_projections.push_back({light->world_position,
                                             {direction.x, direction.y, direction.z},
                                             {up.x, up.y, up.z}});
            }
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
    VsmPageTarget target{framebuffer_, fallback, nullptr};
    std::vector<std::uint64_t> selected;
    if (caster_ids)
    {
        selected.assign(caster_ids->begin(), caster_ids->end());
        target.caster_ids = &selected;
    }
    const bool cacheable = shadow_caster_signature(frame_data, target, candidate_signature_);
    const bool stable = cacheable && candidate_signature_ == caster_signature_;
    if (!cacheable)
    {
        cached_pages_.clear();
        caster_signature_.clear();
        release_solid_batches();
    }
    else if (!stable)
    {
        cached_pages_.clear();
        caster_signature_.swap(candidate_signature_);
        release_solid_batches();
    }

    std::uint32_t reused = 0;
    std::size_t dirty_count = 0;
    std::vector<CachedPage> pending_cache;
    bool dirty_local_pages = false;
    [[maybe_unused]] std::int64_t shared_local_pages = 0;
    [[maybe_unused]] std::int64_t reused_local_pages = 0;
    for (std::size_t index = 0; index < pages.size(); ++index)
    {
        const auto& page = pages[index];
        std::optional<LocalProjection> local_projection;
        if (!caster_ids && index >= cascade_count)
        {
            local_projection = local_projections[index - cascade_count];
        }
        const auto same_projection = [&](const CachedPage& prior)
        {
            ENGINE_PROFILE_FUNCTION_PROBE();
            constexpr auto matrix_bytes = 16 * sizeof(float);
            // Local light coverage is independent of the camera-relative translation.
            const auto view_bytes = local_projection ? 12 * sizeof(float) : matrix_bytes;
            return prior.size == page.size &&
                   (local_projection || prior.caster_origin == frame_data.world_origin) &&
                   prior.local_projection == local_projection &&
                   std::memcmp(prior.view.data(), page.view.data(), view_bytes) == 0 &&
                   std::memcmp(prior.projection.data(), page.projection.data(), matrix_bytes) ==
                       0 &&
                   prior.caster_clip_xy[0] <= page.caster_clip_xy[0] &&
                   prior.caster_clip_xy[1] <= page.caster_clip_xy[1] &&
                   prior.caster_clip_xy[2] >= page.caster_clip_xy[2] &&
                   prior.caster_clip_xy[3] >= page.caster_clip_xy[3];
        };
        const auto cached = std::find_if(cached_pages_.begin(), cached_pages_.end(),
                                         [&](const CachedPage& prior)
                                         {
                                             ENGINE_PROFILE_FUNCTION_PROBE();
                                             return prior.x == page.x && prior.y == page.y &&
                                                    same_projection(prior);
                                         });
        if (cached == cached_pages_.end())
        {
            ++dirty_count;
            dirty_local_pages |= index >= cascade_count;
            if (cacheable)
            {
                pending_cache.push_back({page.x, page.y, page.size, page.view_id, page.view,
                                         page.projection, page.caster_clip_xy,
                                         frame_data.world_origin, local_projection});
            }
            continue;
        }
        reused |= std::uint32_t{1} << index;
        if (index >= cascade_count)
        {
            ++reused_local_pages;
            shared_local_pages += cached->producer_view != page.view_id;
        }
    }
    ENGINE_PROFILE_PLOT("Render.shadowCacheHit", static_cast<std::int64_t>(dirty_count == 0));
    ENGINE_PROFILE_PLOT("Render.shadowSubmittedPages", static_cast<std::int64_t>(dirty_count));
    ENGINE_PROFILE_PLOT("Render.shadowSharedLocalPages", shared_local_pages);
    ENGINE_PROFILE_PLOT("Render.shadowReusedLocalPages", reused_local_pages);
    if (cacheable && !caster_ids && base_view == shadow_view)
    {
        // Reflections can see the same inputs earlier in one frame. Require the
        // signature to survive between two main-view submissions before uploading.
        if (solid_inputs_seen_ && dirty_count != 0 && solid_sources_.empty() &&
            bgfx::isValid(solid_batch_program_) && frame_data.meshes.size() > 1)
        {
            prepare_solid_batches(frame_data);
        }
        solid_inputs_seen_ = true;
    }
    // A failed partial encode also invalidates reductions derived from the old local depth.
    local_depth_revision_ += dirty_local_pages;
    if (!encode_pages(frame_data, pages, reused, target, observer_tiles_[observer]))
    {
        return false;
    }
    cached_pages_.insert(cached_pages_.end(), pending_cache.begin(), pending_cache.end());
    return true;
}

void ShadowPass::release_solid_batches() noexcept
{
    ENGINE_PROFILE_FUNCTION();
    for (const auto& batch : solid_batches_)
    {
        bgfx::destroy(batch.indices);
        bgfx::destroy(batch.positions);
    }
    solid_batches_.clear();
    solid_sources_.clear();
    solid_batch_bytes_ = 0;
    solid_inputs_seen_ = false;
}

void ShadowPass::prepare_solid_batches(const RenderFrameData& frame)
{
    ENGINE_PROFILE_ZONE("Render.shadowPrepareSolidBatches");
    using Position = std::array<float, 3>;
    static_assert(sizeof(Position) == 12);
    constexpr std::size_t vertex_limit = std::size_t{UINT16_MAX} + 1;
    constexpr std::size_t memory_limit = 16 * 1024 * 1024;
    using GroupKey = std::pair<std::array<std::uint64_t, 16>, std::uint64_t>;
    std::map<GroupKey, std::vector<std::size_t>> groups;
    solid_sources_.resize(frame.meshes.size());
    for (std::size_t index = 0; index < frame.meshes.size(); ++index)
    {
        const auto& source = frame.meshes[index];
        if (!source.cast_shadows || !source.all_opaque || !source.instance_eligible ||
            source.instancing || !source.gpu || !source.data || source.gpu->materials.empty())
        {
            continue;
        }
        const auto& data = *source.data;
        const auto topology = mesh_storage::indices(data);
        // Use the cache for smaller draw-heavy participants. Large meshes keep their own buffers.
        if (data.vertices.empty() || data.vertices.size() > vertex_limit / 16 || topology.empty() ||
            topology.size() % 3 != 0 || topology.size() > memory_limit / sizeof(std::uint16_t))
        {
            continue;
        }
        const auto& materials = source.gpu->materials;
        const auto first_slot = data.submeshes.empty() ? 0 : data.submeshes.front().material_slot();
        if (first_slot >= materials.size())
        {
            continue;
        }
        const auto cull = materials[first_slot].cull;
        const auto compatible = [&](std::size_t slot)
        {
            ENGINE_PROFILE_FUNCTION_PROBE();
            return slot < materials.size() && materials[slot].shadow_valid &&
                   materials[slot].blend == schema::BlendMode::opaque &&
                   materials[slot].shadow_alpha[0] <= 0 && materials[slot].cull == cull;
        };
        if (!compatible(first_slot))
        {
            continue;
        }
        std::size_t total_indices = 0;
        const bool complete = std::ranges::all_of(
            data.submeshes,
            [&](const auto& range)
            {
                ENGINE_PROFILE_FUNCTION_PROBE();
                if (range.first_index() != total_indices || range.index_count() % 3 != 0 ||
                    range.index_count() > topology.size() - total_indices ||
                    !compatible(range.material_slot()))
                {
                    return false;
                }
                total_indices += range.index_count();
                return true;
            });
        const auto valid_vertex = [&](std::uint32_t value)
        {
            ENGINE_PROFILE_FUNCTION_PROBE();
            return value < data.vertices.size();
        };
        if (!complete || (!data.submeshes.empty() && total_indices != topology.size()) ||
            !std::ranges::all_of(topology.wide, valid_vertex) ||
            !std::ranges::all_of(topology.narrow, valid_vertex))
        {
            continue;
        }
        const auto matrix = std::bit_cast<std::array<std::uint64_t, 16>>(source.world);
        const auto state =
            BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS | cull_state(cull, source.model.data());
        groups[{matrix, state}].push_back(index);
    }
    bgfx::VertexLayout layout;
    layout.begin().add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float).end();
    std::vector<Position> positions;
    std::vector<std::uint16_t> indices;
    std::vector<SolidRange> ranges;
    const auto upload = [&](std::uint64_t state)
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        if (ranges.size() > 1)
        {
            const auto vertex_bytes = positions.size() * sizeof(Position);
            const auto index_bytes = indices.size() * sizeof(std::uint16_t);
            const auto vertex = bgfx::createVertexBuffer(
                bgfx::copy(positions.data(), static_cast<std::uint32_t>(vertex_bytes)), layout);
            if (bgfx::isValid(vertex))
            {
                const auto index = bgfx::createIndexBuffer(
                    bgfx::copy(indices.data(), static_cast<std::uint32_t>(index_bytes)));
                if (bgfx::isValid(index))
                {
                    for (const auto& range : ranges)
                    {
                        solid_sources_[range.source] = 1;
                    }
                    solid_batch_bytes_ += vertex_bytes + index_bytes;
                    solid_batches_.push_back({vertex, index, state, std::move(ranges)});
                }
                else
                {
                    bgfx::destroy(vertex);
                }
            }
        }
        positions.clear();
        indices.clear();
        ranges.clear();
    };
    for (auto& [key, group] : groups)
    {
        if (group.size() < 2)
        {
            continue;
        }
        std::ranges::sort(group, {},
                          [&](std::size_t index)
                          {
                              ENGINE_PROFILE_FUNCTION_PROBE();
                              return frame.meshes[index].center;
                          });
        for (const auto source : group)
        {
            const auto& data = *frame.meshes[source].data;
            const auto topology = mesh_storage::indices(data);
            if (positions.size() + data.vertices.size() > vertex_limit)
            {
                upload(key.second);
            }
            const auto bytes = (positions.size() + data.vertices.size()) * sizeof(Position) +
                               (indices.size() + topology.size()) * sizeof(std::uint16_t);
            if (bytes > memory_limit - solid_batch_bytes_)
            {
                continue;
            }
            const auto base = positions.size();
            const auto draws = std::max<std::size_t>(1, data.submeshes.size());
            ranges.push_back({source, static_cast<std::uint32_t>(indices.size()),
                              static_cast<std::uint32_t>(topology.size()),
                              static_cast<std::uint32_t>(draws)});
            for (const auto& point : data.vertices)
            {
                positions.push_back({point.x(), point.y(), point.z()});
            }
            for (std::size_t index = 0; index < topology.size(); ++index)
            {
                indices.push_back(static_cast<std::uint16_t>(base + topology[index]));
            }
        }
        upload(key.second);
    }
}

bool ShadowPass::TileGraph::prepare(std::span<const VsmPageDraw> pages, std::uint32_t cached_mask)
{
    if (pages.size() > tile_capacity)
    {
        return false;
    }
    const auto tile = [](const VsmPageDraw& p) { return AtlasTile{p.view_id, p.x, p.y, p.size}; };
    if (reused == cached_mask && count == pages.size() &&
        std::equal(pages.begin(), pages.end(), layout.begin(),
                   [&](const auto& draw, const auto& prior) { return tile(draw) == prior; }) &&
        plan.begin_cached_frame())
    {
        return true;
    }
    // One version names one region of the atlas. The allocator gives each consumer a disjoint
    // region; a cache hit imports that region from its preceding producer, even across observers.
    constexpr FrameResourceId caster_streams = 0;
    constexpr FrameResourceId reads[]{caster_streams};
    std::array<FrameResourceId, tile_capacity + 1> imports{caster_streams};
    std::array<FrameResourceId, tile_capacity> regions{};
    std::array<FramePassDeclaration, tile_capacity> passes{};
    std::size_t import_count = 1, writers = 0;
    for (std::size_t i = 0; i < pages.size(); ++i)
    {
        const auto& page = pages[i];
        if (page.view_id >= frame_pass_capacity || page.size == 0 ||
            std::any_of(pages.begin(), pages.begin() + i, [&](const auto& prior)
                        { return page.view_id == prior.view_id || overlaps(page, prior); }))
        {
            return false;
        }
        regions[i] = static_cast<FrameResourceId>(1 + page.view_id);
        if ((cached_mask & (std::uint32_t{1} << i)) != 0)
        {
            imports[import_count++] = regions[i];
        }
        else
        {
            passes[writers++] = {static_cast<FramePassId>(i), reads,
                                 std::span(regions).subspan(i, 1)};
        }
    }
    FramePlanError error;
    if (!plan.compile(std::span(passes).first(writers), std::span(imports).first(import_count),
                      std::span(regions).first(pages.size()), error))
    {
        return false;
    }
    std::transform(pages.begin(), pages.end(), layout.begin(), tile);
    reused = cached_mask;
    count = pages.size();
    return true;
}

bool ShadowPass::draw_pages(const RenderFrameData& frame_data, std::span<const VsmPageDraw> pages,
                            const VsmPageTarget& target)
{
    ENGINE_PROFILE_FUNCTION();
    return encode_pages(frame_data, pages, 0, target, virtual_tiles_);
}

bool ShadowPass::encode_pages(const RenderFrameData& frame_data, std::span<const VsmPageDraw> pages,
                              std::uint32_t reused, const VsmPageTarget& target, TileGraph& graph)
{
    ENGINE_PROFILE_FUNCTION();
    if (!graph.prepare(pages, reused))
    {
        return false;
    }
    FramePlanError error;
    if (graph.plan.passes().empty())
    {
        return graph.plan.perform([](FramePassId) { return false; }, error);
    }
    if (target.framebuffer.idx == framebuffer_.idx)
    {
        std::erase_if(cached_pages_,
                      [&](const CachedPage& cached)
                      {
                          return std::any_of(graph.plan.passes().begin(), graph.plan.passes().end(),
                                             [&](FramePassId writer)
                                             { return overlaps(pages[writer], cached); });
                      });
    }
    if (!bgfx::isValid(target.framebuffer) || !bgfx::isValid(program_) ||
        !bgfx::isValid(target.fallback))
    {
        return false;
    }
    const auto* caps = bgfx::getCaps();
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
        if (!source.cast_shadows || !source.any_opaque || !source.gpu ||
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
        ENGINE_PROFILE_FUNCTION_PROBE();
        const auto& mesh = *caster.mesh;
        const auto* data = caster.source->data;
        const auto collect = [&](std::uint32_t first, std::uint32_t count, std::size_t slot)
        {
            ENGINE_PROFILE_FUNCTION_PROBE();
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
            key.state = BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS |
                        cull_state(material.cull, caster.model.data());
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
            !collect(0, static_cast<std::uint32_t>(mesh_storage::indices(*data).size()), 0))
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
    const bool use_packed =
        target.framebuffer.idx == framebuffer_.idx && !target.caster_ids && !solid_batches_.empty();
    std::vector<std::uint8_t> packed_visible(use_packed ? solid_sources_.size() : 0);
    [[maybe_unused]] std::int64_t solid_draws = 0;
    [[maybe_unused]] std::int64_t packed_draws = 0;
    [[maybe_unused]] std::int64_t packed_original_draws = 0;
    const auto submit_packed = [&](bgfx::ViewId view)
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        for (const auto& batch : solid_batches_)
        {
            std::size_t begin = 0;
            while (begin < batch.ranges.size())
            {
                const auto& first = batch.ranges[begin++];
                if (!packed_visible[first.source])
                {
                    continue;
                }
                auto count = first.count;
                packed_original_draws += first.original_draws;
                while (begin < batch.ranges.size() && packed_visible[batch.ranges[begin].source])
                {
                    const auto& range = batch.ranges[begin++];
                    count += range.count;
                    packed_original_draws += range.original_draws;
                }
                bgfx::setTransform(frame_data.meshes[first.source].model.data());
                bgfx::setVertexBuffer(0, batch.positions);
                bgfx::setIndexBuffer(batch.indices, first.first, count);
                bgfx::setState(batch.state);
                bgfx::submit(view, solid_batch_program_);
                ++packed_draws;
                ++solid_draws;
            }
        }
        std::ranges::fill(packed_visible, std::uint8_t{0});
    };
    const auto submit_batches = [&](bgfx::ViewId view)
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        for (auto& batch : batches)
        {
            while (batch.cursor < batch.items.size())
            {
                const auto& item = items[batch.items[batch.cursor]];
                const auto& caster = *item.caster;
                const bool solid = item.material->shadow_alpha[0] <= 0.0F;
                if (!solid && !bind_material(*item.material, target.fallback))
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
                caster.mesh->bind_vertices();
                bgfx::setIndexBuffer(caster.mesh->geometry->index_buffer(), batch.key.first,
                                     batch.key.count);
                bgfx::setState(batch.key.state);
                const auto regular = solid ? solid_program_ : program_;
                const auto instanced = solid ? instanced_solid_program_ : instanced_program_;
                bgfx::submit(view, count > 1 ? instanced : regular);
                solid_draws += solid;
            }
        }
        items.clear();
        batches.clear();
        batch_index.clear();
        return true;
    };

    const auto submit_page = [&](FramePassId writer)
    {
        const auto& page = pages[writer];
        if (!page.size || page.view_id >= caps->limits.maxViews ||
            !finite_matrix(page.view.data()) || !finite_matrix(page.projection.data()))
        {
            return false;
        }
        bgfx::setViewName(page.view_id, "Shadow page");
        bgfx::setViewFrameBuffer(page.view_id, target.framebuffer);
        bgfx::setViewRect(page.view_id, page.x, page.y, page.size, page.size);
        bgfx::setViewClear(page.view_id, BGFX_CLEAR_DEPTH, 0, 1.F, 0);
        bgfx::setViewTransform(page.view_id, page.view.data(), page.projection.data());
        if (target.virtual_pages && !target.virtual_pages->begin_page_write(target.virtual_batch, page))
        {
            return false;
        }
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
                    (clip[0] < page.caster_clip_xy[0] * clip[3] ? 1U : 0U) |
                    (clip[0] > page.caster_clip_xy[2] * clip[3] ? 2U : 0U) |
                    (clip[1] < page.caster_clip_xy[1] * clip[3] ? 4U : 0U) |
                    (clip[1] > page.caster_clip_xy[3] * clip[3] ? 8U : 0U) |
                    (clip[2] < (caps->homogeneousDepth ? -clip[3] : 0.F) ? 16U : 0U) |
                    (clip[2] > clip[3] ? 32U : 0U);
                outside &= mask;
                if (!outside)
                {
                    break;
                }
            }
            if (outside)
            {
                continue;
            }
            const auto source = static_cast<std::size_t>(caster.source - frame_data.meshes.data());
            if (use_packed && solid_sources_[source])
            {
                packed_visible[source] = 1;
                continue;
            }
            if (!collect_caster(caster))
            {
                return false;
            }
        }
        if (!submit_batches(page.view_id))
        {
            return false;
        }
        if (use_packed)
        {
            submit_packed(page.view_id);
        }
        return true;
    };
    if (!graph.plan.perform(submit_page, error))
    {
        return false;
    }
    ENGINE_PROFILE_PLOT("Render.shadowSolidDraws", solid_draws);
    if (use_packed)
    {
        ENGINE_PROFILE_PLOT("Render.shadowPackedDraws", packed_draws);
        ENGINE_PROFILE_PLOT("Render.shadowPackedOriginalDraws", packed_original_draws);
        ENGINE_PROFILE_PLOT("Render.shadowPackedBytes",
                            static_cast<std::int64_t>(solid_batch_bytes_));
    }
    return true;
}

} // namespace engine::render_detail
