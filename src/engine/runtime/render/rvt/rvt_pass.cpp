#include "runtime/render/rvt/rvt_pass.hpp"
#include "runtime/performance/function_profile.hpp"
#include "runtime/performance/profiling.hpp"
#include "runtime/render/render_constants.hpp"
#include "runtime/render/render_math.hpp"
#include "runtime/render/resources/gpu_program.hpp"
#include "runtime/utils/transform_math.hpp"
#include <algorithm>
#include <bx/math.h>
#include <cmath>
#include <limits>

namespace engine::render_detail
{
namespace
{
bool writer(schema::RvtRole role)
{
    ENGINE_PROFILE_FUNCTION();
    return role == schema::RvtRole::writer || role == schema::RvtRole::writer_reader;
}
Bounds3 mesh_bounds(const RenderFrameData::Mesh& mesh)
{
    ENGINE_PROFILE_FUNCTION();
    std::array<double, 3> low{INFINITY, INFINITY, INFINITY}, high{-INFINITY, -INFINITY, -INFINITY};
    for (unsigned corner = 0; corner < 8; ++corner)
    {
        std::array<float, 3> local{};
        for (unsigned axis = 0; axis < 3; ++axis)
        {
            local[axis] =
                (corner & (1U << axis)) ? mesh.gpu->maximum[axis] : mesh.gpu->minimum[axis];
        }
        const auto world = transform_math::point(mesh.world.data(), {local[0], local[1], local[2]});
        const double position[]{world.x(), world.y(), world.z()};
        for (unsigned axis = 0; axis < 3; ++axis)
        {
            low[axis] = std::min(low[axis], position[axis]);
            high[axis] = std::max(high[axis], position[axis]);
        }
    }
    return {{low[0], low[1], low[2]}, {high[0], high[1], high[2]}};
}
bool overlap(const Bounds3& a, const Bounds3& b)
{
    ENGINE_PROFILE_FUNCTION();
    return a.minimum().x() <= b.maximum().x() && a.maximum().x() >= b.minimum().x() &&
           a.minimum().z() <= b.maximum().z() && a.maximum().z() >= b.minimum().z();
}
} // namespace
RvtPass::~RvtPass() noexcept
{
    ENGINE_PROFILE_FUNCTION();
    release_gpu();
}
bool RvtPass::configure(const schema::RvtSettingsT& settings, std::uint64_t device)
{
    ENGINE_PROFILE_FUNCTION();
    if (available() && settings_ == settings && device_ == device)
    {
        return true;
    }
    release_gpu();
    settings_ = settings;
    device_ = device;
    if (!valid_rvt_settings(settings))
    {
        reason_ = "Invalid RVT settings";
        return false;
    }
    const auto* caps = bgfx::getCaps();
    if (!caps || caps->limits.maxFBAttachments < 4 || caps->limits.maxTextureSamplers < 10 ||
        !bgfx::isTextureValid(0, false, 3, bgfx::TextureFormat::RGBA16F, BGFX_TEXTURE_RT) ||
        !bgfx::isTextureValid(0, false, 1, bgfx::TextureFormat::RGBA32F, 0))
    {
        reason_ = "RVT requires array render targets and float page tables";
        return false;
    }
    slots_per_row_ = std::uint32_t(std::ceil(std::sqrt(settings.physical_pages)));
    atlas_size_ = slots_per_row_ * (settings.page_size + 2 * settings.page_border);
    if (atlas_size_ > caps->limits.maxTextureSize)
    {
        reason_ = "RVT atlas exceeds the device texture limit";
        return false;
    }
    const auto flags = BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
    atlas_ = bgfx::createTexture2D(std::uint16_t(atlas_size_), std::uint16_t(atlas_size_), false, 3,
                                   bgfx::TextureFormat::RGBA16F, flags);
    depth_ = bgfx::createTexture2D(std::uint16_t(atlas_size_), std::uint16_t(atlas_size_), false, 1,
                                   bgfx::TextureFormat::D32F, BGFX_TEXTURE_RT_WRITE_ONLY);
    if (!bgfx::isValid(atlas_) || !bgfx::isValid(depth_))
    {
        release_gpu();
        reason_ = "RVT atlas allocation failed";
        return false;
    }
    bgfx::Attachment attachments[4];
    for (std::uint16_t layer = 0; layer < 3; ++layer)
    {
        attachments[layer].init(atlas_, bgfx::Access::Write, layer, 1, 0, 0);
    }
    attachments[3].init(depth_, bgfx::Access::Write, 0, 1, 0, 0);
    target_ = bgfx::createFrameBuffer(4, attachments, false);
    const auto grid = std::uint16_t(settings.virtual_resolution / settings.page_size);
    table_ =
        bgfx::createTexture2D(grid, grid, true, 1, bgfx::TextureFormat::RGBA32F,
                              BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP | BGFX_SAMPLER_MIN_POINT |
                                  BGFX_SAMPLER_MAG_POINT | BGFX_SAMPLER_MIP_POINT);
    atlas_sampler_ = bgfx::createUniform("s_rvt_atlas", bgfx::UniformType::Sampler);
    table_sampler_ = bgfx::createUniform("s_rvt_table", bgfx::UniformType::Sampler);
    bounds_ = bgfx::createUniform("u_rvt_bounds", bgfx::UniformType::Vec4);
    layout_ = bgfx::createUniform("u_rvt_layout", bgfx::UniformType::Vec4);
    parameters_ = bgfx::createUniform("u_rvt_parameters", bgfx::UniformType::Vec4);
    height_ = bgfx::createUniform("u_rvt_height", bgfx::UniformType::Vec4);
    normal_ = bgfx::createUniform("u_normal_transform", bgfx::UniformType::Mat4);
    surface_ = bgfx::createUniform("u_surface", bgfx::UniformType::Vec4);
    if (!bgfx::isValid(target_) || !bgfx::isValid(table_) || !bgfx::isValid(surface_))
    {
        release_gpu();
        reason_ = "RVT GPU initialization failed";
        return false;
    }
    initialize_atlas_ = true;
    return true;
}
void RvtPass::invalidate_sources(VirtualTextureCache& cache, const RenderFrameData& frame)
{
    ENGINE_PROFILE_ZONE("Render.rvtInvalidateSources");
    std::map<std::uint64_t, Source> next;
    for (const auto& mesh : frame.meshes)
    {
        if (!writer(mesh.rvt_role))
        {
            continue;
        }
        Source value{
            mesh.gpu->instance_id, mesh.gpu->revision, mesh.gpu->deformation_revision, mesh.world,
            mesh.gpu->materials,   mesh_bounds(mesh)};
        const auto old = sources_.find(mesh.gpu->node_id);
        if (old == sources_.end() || old->second.instance != value.instance ||
            old->second.revision != value.revision ||
            old->second.deformation != value.deformation || old->second.model != value.model ||
            old->second.materials != value.materials)
        {
            if (old != sources_.end())
            {
                cache.invalidate(old->second.bounds, ++source_revision_);
            }
            cache.invalidate(value.bounds, ++source_revision_);
        }
        // Store copied parameters/handles as identity only; never dereference cached program
        // pointers.
        next.emplace(mesh.gpu->node_id, std::move(value));
    }
    for (const auto& [id, value] : sources_)
    {
        if (!next.contains(id))
        {
            cache.invalidate(value.bounds, ++source_revision_);
        }
    }
    sources_ = std::move(next);
}
bool RvtPass::prepare_pages(VirtualTextureCache& cache, const RenderFrameData& sources)
{
    ENGINE_PROFILE_ZONE("Render.rvtPageSubmit");
    if (!available())
    {
        return false;
    }
    std::vector<const RenderFrameData::Mesh*> writers;
    for (const auto& mesh : sources.meshes)
    {
        if (writer(mesh.rvt_role))
        {
            for (const auto& material : mesh.gpu->materials)
            {
                if (!material.program || !bgfx::isValid(material.program->rvt_program) ||
                    material.frame_textures || material.blend != schema::BlendMode::opaque ||
                    material.unlit)
                {
                    reason_ = "An RVT writer has no opaque PBR cache variant";
                    return false;
                }
            }
            writers.push_back(&mesh);
        }
    }
    std::sort(writers.begin(), writers.end(),
              [](auto a, auto b)
              {
                  ENGINE_PROFILE_FUNCTION_PROBE();
                  return a->gpu->node_id < b->gpu->node_id;
              });
    const auto& batch = cache.batch();
    if (batch.device_generation != device_ || batch.pages.size() > maximum_rvt_views)
    {
        return false;
    }
    const auto levels = static_cast<std::uint8_t>(cache.maximum_mip() + 1);
    if (!prepare_plan(static_cast<std::uint16_t>(batch.pages.size()), levels))
    {
        return false;
    }
    FramePlanError error;
    const bool success = plan_.perform(
        [&](FramePassId pass)
        {
            if (pass == 0)
            {
                const auto clear_view = bgfx::ViewId(rvt_page_view - 1);
                bgfx::setViewName(clear_view, "RVT atlas initialize");
                bgfx::setViewFrameBuffer(clear_view, target_);
                bgfx::setViewRect(clear_view, 0, 0, std::uint16_t(atlas_size_),
                                  std::uint16_t(atlas_size_));
                bgfx::setViewClear(clear_view, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH,
                                   std::uint32_t{0}, 1.F);
                bgfx::touch(clear_view);
                initialize_atlas_ = false;
                return true;
            }
            if (pass <= maximum_rvt_views)
            {
                const auto& page = batch.pages[pass - 1];
                return cache.begin_page_write(page) &&
                       draw_page(page, cache.bounds(page.key), rvt_page_view + pass - 1, writers);
            }
            if (pass == maximum_rvt_views + 1)
            {
                return cache.publish(batch);
            }
            upload_table_level(cache, static_cast<std::uint8_t>(pass - maximum_rvt_views - 2));
            return true;
        },
        error);
    if (!success)
    {
        reason_ = error.reason;
        return false;
    }
    ENGINE_PROFILE_PLOT("Rvt.redrawnPages", static_cast<std::int64_t>(batch.pages.size()));
    minimum_mip_ = cache.maximum_mip();
    for (const auto key : cache.demand())
    {
        minimum_mip_ = std::min(minimum_mip_, key.mip);
    }
    reason_.clear();
    return true;
}

bool RvtPass::prepare_plan(std::uint16_t pages, std::uint8_t levels)
{
    if (pages == planned_pages_ && levels == planned_levels_ && planned_clear_ == initialize_atlas_)
    {
        return plan_.begin_cached_frame();
    }
    // Each candidate slot is distinct. Its tile and the imported or cleared atlas are inputs
    // to publication. Each table mip is a separate upload; bgfx applies uploads before views.
    constexpr FramePassId publish = maximum_rvt_views + 1;
    constexpr FramePassId first_mip = publish + 1;
    constexpr unsigned max_levels =
        std::numeric_limits<decltype(settings_.virtual_resolution)>::digits;
    if (pages > maximum_rvt_views || levels == 0 || levels > max_levels)
    {
        reason_ = "RVT page plan exceeds its declaration capacity";
        return false;
    }
    std::array<FrameResourceId, first_mip + max_levels> versions{};
    std::array<FramePassDeclaration, first_mip + max_levels> passes{};
    for (FrameResourceId i = 0; i < versions.size(); ++i)
    {
        versions[i] = i;
    }
    const std::span ids{versions};
    constexpr FrameResourceId writer_stream = first_mip + max_levels;
    constexpr std::array<FrameResourceId, 2> page_inputs{0, writer_stream};
    const std::array imports{writer_stream, FrameResourceId{0}};
    std::size_t count = 0;
    if (initialize_atlas_)
    {
        passes[count++] = {0, {}, ids.first(1)};
    }
    for (FramePassId i = 1; i <= pages; ++i)
    {
        passes[count++] = {i, page_inputs, ids.subspan(i, 1)};
    }
    passes[count++] = {publish, ids.first(pages + 1), ids.subspan(publish, 1)};
    for (FramePassId i = first_mip; i < first_mip + levels; ++i)
    {
        passes[count++] = {i, ids.subspan(publish, 1), ids.subspan(i, 1)};
    }
    FramePlanError error;
    planned_pages_ = frame_invalid_id;
    if (!plan_.compile(std::span(passes).first(count),
                       std::span(imports).first(initialize_atlas_ ? 1 : 2),
                       ids.subspan(first_mip, levels), error))
    {
        reason_ = error.reason;
        return false;
    }
    planned_pages_ = pages;
    planned_levels_ = levels;
    planned_clear_ = initialize_atlas_;
    return true;
}

bool RvtPass::draw_page(const RvtPage& page, const Bounds3& area, bgfx::ViewId id,
                        std::span<const RenderFrameData::Mesh* const> writers)
{
    const auto tile = std::uint16_t(settings_.page_size + 2 * settings_.page_border);
    const auto& a = area.minimum();
    const auto& b = area.maximum();
    const float border_x = float((b.x() - a.x()) * settings_.page_border / settings_.page_size);
    const float border_z = float((b.z() - a.z()) * settings_.page_border / settings_.page_size);
    const Vec3 origin{(a.x() + b.x()) * .5, b.y() + 1, (a.z() + b.z()) * .5};
    const bx::Vec3 eye{0, 0, 0};
    const bx::Vec3 at{0, float(a.y() - origin.y()), 0};
    float view[16], projection[16];
    // X increases right, Z increases down in page-table texture coordinates.
    bx::mtxLookAt(view, eye, at, {0, 0, -1}, bx::Handedness::Right);
    const float half_x = float((b.x() - a.x()) * .5) + border_x,
                half_z = float((b.z() - a.z()) * .5) + border_z;
    bx::mtxOrtho(projection, -half_x, half_x, -half_z, half_z, .1F, float(b.y() - a.y() + 2), 0,
                 bgfx::getCaps()->homogeneousDepth, bx::Handedness::Right);
    bgfx::setViewName(id, "RVT material page");
    bgfx::setViewFrameBuffer(id, target_);
    bgfx::setViewRect(id, std::uint16_t(page.slot % slots_per_row_ * tile),
                      std::uint16_t(page.slot / slots_per_row_ * tile), tile, tile);
    bgfx::setViewClear(id, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, std::uint32_t{0}, 1.F);
    bgfx::setViewTransform(id, view, projection);
    bgfx::setViewMode(id, bgfx::ViewMode::Sequential);
    bgfx::touch(id);
    for (const auto* mesh : writers)
    {
        const Bounds3 expanded{{a.x() - border_x, a.y(), a.z() - border_z},
                               {b.x() + border_x, b.y(), b.z() + border_z}};
        if (!overlap(mesh_bounds(*mesh), expanded))
        {
            continue;
        }
        const auto& geometry = *mesh->gpu->geometry;
        const auto submeshes = geometry.submeshes();
        const auto count = submeshes.empty() ? 1 : submeshes.size();
        for (std::size_t slot = 0; slot < count; ++slot)
        {
            const auto& material =
                mesh->gpu->materials[submeshes.empty() ? 0 : submeshes[slot].material_slot()];
            if (!material.program->bind_material(material, {}, false))
            {
                return false;
            }
            const float height[]{
                float(settings_.bounds.minimum().y() - origin.y()),
                float(1 / (settings_.bounds.maximum().y() - settings_.bounds.minimum().y())), 0, 0};
            const float surface[]{0, 0, 0, 0};
            bgfx::setUniform(height_, height);
            bgfx::setUniform(surface_, surface);
            float model[16];
            transform_math::relative(mesh->world.data(), origin, model);
            bgfx::setTransform(model);
            bgfx::setUniform(normal_, mesh->normal.data());
            mesh->gpu->bind_vertices();
            const auto first = submeshes.empty() ? 0 : submeshes[slot].first_index();
            const auto indices =
                submeshes.empty() ? geometry.index_count() : submeshes[slot].index_count();
            bgfx::setIndexBuffer(geometry.index_buffer(), first, indices);
            bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z |
                           BGFX_STATE_DEPTH_TEST_LESS | cull_state(material.cull, model));
            bgfx::submit(id, material.program->rvt_program);
        }
    }
    return true;
}

void RvtPass::upload_table_level(const VirtualTextureCache& cache, std::uint8_t mip)
{
    const auto count = std::uint16_t((settings_.virtual_resolution / settings_.page_size) >> mip);
    std::vector<std::array<float, 4>> data(std::size_t(count) * count);
    for (const auto& [key, page] : cache.pages())
    {
        if (key.mip == mip)
        {
            data[std::size_t(key.y) * count + key.x] = {float(page.slot % slots_per_row_),
                                                        float(page.slot / slots_per_row_),
                                                        float(mip), 1};
        }
    }
    bgfx::updateTexture2D(table_, 0, mip, 0, 0, count, count,
                          bgfx::copy(data.data(), std::uint32_t(data.size() * sizeof(data[0]))));
}
void RvtPass::bind(bool receiver, const Vec3& origin) const
{
    ENGINE_PROFILE_FUNCTION();
    if (!available())
    {
        bind_disabled();
        return;
    }
    const auto& a = settings_.bounds.minimum();
    const auto& b = settings_.bounds.maximum();
    const float bounds[]{float(a.x() - origin.x()), float(a.z() - origin.z()),
                         float(1 / (b.x() - a.x())), float(1 / (b.z() - a.z()))};
    const float layout[]{float(settings_.page_size), float(settings_.page_border),
                         float(atlas_size_),
                         float(settings_.virtual_resolution / settings_.page_size)};
    const float parameters[]{receiver ? 1.F : 0.F,
                             float(std::log2(settings_.virtual_resolution / settings_.page_size)),
                             float(minimum_mip_), float(diagnostics_)};
    const float height[]{float(a.y() - origin.y()), float(1 / (b.y() - a.y())), 0, 0};
    bgfx::setUniform(bounds_, bounds);
    bgfx::setUniform(layout_, layout);
    bgfx::setUniform(parameters_, parameters);
    bgfx::setUniform(height_, height);
    bgfx::setTexture(8, atlas_sampler_, atlas_);
    bgfx::setTexture(9, table_sampler_, table_);
}
void RvtPass::bind_disabled()
{
    ENGINE_PROFILE_FUNCTION();
    const auto handle = bgfx::createUniform("u_rvt_parameters", bgfx::UniformType::Vec4);
    const float parameters[]{0, 0, 0, 0};
    bgfx::setUniform(handle, parameters);
    bgfx::destroy(handle);
}
std::uint64_t RvtPass::resident_bytes() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return available() ? std::uint64_t(atlas_size_) * atlas_size_ * 28 +
                             std::uint64_t(settings_.virtual_resolution / settings_.page_size) *
                                 (settings_.virtual_resolution / settings_.page_size) * 64 / 3
                       : 0;
}
void RvtPass::release_gpu() noexcept
{
    ENGINE_PROFILE_FUNCTION();
    if (bgfx::isValid(target_))
    {
        bgfx::destroy(target_);
    }
    if (bgfx::isValid(atlas_))
    {
        bgfx::destroy(atlas_);
    }
    if (bgfx::isValid(table_))
    {
        bgfx::destroy(table_);
    }
    if (bgfx::isValid(depth_))
    {
        bgfx::destroy(depth_);
    }
    for (auto handle : {atlas_sampler_, table_sampler_, bounds_, layout_, parameters_, height_,
                        normal_, surface_})
    {
        if (bgfx::isValid(handle))
        {
            bgfx::destroy(handle);
        }
    }
    target_ = BGFX_INVALID_HANDLE;
    atlas_ = table_ = depth_ = BGFX_INVALID_HANDLE;
    atlas_sampler_ = table_sampler_ = bounds_ = layout_ = parameters_ = height_ = normal_ =
        surface_ = BGFX_INVALID_HANDLE;
    sources_.clear();
    device_ = 0;
    atlas_size_ = slots_per_row_ = 0;
    reason_.clear();
}
} // namespace engine::render_detail
