#include "runtime/render/passes/clustered_lighting_pass.hpp"
#include "runtime/performance/profiling.hpp"
#include "runtime/render/render_math.hpp"
#include "runtime/render/render_system.hpp"

#include "cs_cluster_cull.bin.h"

#include <SDL3/SDL_log.h>
#include <bx/math.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace engine::render_detail
{

static_assert(sizeof(ClusterLight) == sizeof(float) * 16);
static_assert(sizeof(ShadowRecord) == sizeof(float) * 24);

ClusteredLightingPass::~ClusteredLightingPass() noexcept
{
    release();
}

bool ClusteredLightingPass::prepare(const RenderConfig& config, std::uint16_t width,
                                    std::uint16_t height, std::uint32_t vsm_vectors)
{
    const auto* caps = bgfx::getCaps();
    if (caps == nullptr || (caps->supported & BGFX_CAPS_COMPUTE) == 0 ||
        caps->limits.maxComputeBindings < 2 || caps->limits.maxTextureSamplers < 16 || width == 0 ||
        height == 0 || config.cluster_tile_size < 16 || config.cluster_tile_size > 256 ||
        config.cluster_depth_slices == 0 || config.cluster_depth_slices > 64 ||
        config.maximum_lights == 0 || config.maximum_lights > 4096 ||
        config.maximum_cluster_lights == 0 || config.maximum_cluster_lights > 256 ||
        config.maximum_local_shadow_faces > 24)
    {
        return false;
    }
    const auto x = (static_cast<std::uint32_t>(width) + config.cluster_tile_size - 1) /
                   config.cluster_tile_size;
    const auto y = (static_cast<std::uint32_t>(height) + config.cluster_tile_size - 1) /
                   config.cluster_tile_size;
    const std::uint64_t count = std::uint64_t{x} * y * config.cluster_depth_slices;
    // Each cluster stores a count followed by two light indices per 32-bit word.
    const std::uint64_t words_per_cluster =
        1 + (std::uint64_t{config.maximum_cluster_lights} + 1) / 2;
    const std::uint64_t cluster_words = count * words_per_cluster;
    const std::uint64_t vectors = std::uint64_t{config.maximum_lights} * 4 +
                                  std::uint64_t{config.maximum_local_shadow_faces} * 6 +
                                  vsm_vectors;
    // Uniform counters are exactly representable, and buffer byte sizes fit bgfx's API.
    if (count == 0 || count > 16777216 || cluster_words > UINT32_MAX / sizeof(std::uint32_t) ||
        vectors > UINT32_MAX / (sizeof(float) * 4) || (count + 63) / 64 > 65535)
    {
        return false;
    }
    const std::array<float, 4> grid = {static_cast<float>(x), static_cast<float>(y),
                                       static_cast<float>(config.cluster_depth_slices),
                                       static_cast<float>(config.cluster_tile_size)};
    if (vsm_vectors_ == vsm_vectors && grid_ == grid && maximum_lights_ == config.maximum_lights &&
        maximum_cluster_lights_ == config.maximum_cluster_lights &&
        maximum_shadow_faces_ == config.maximum_local_shadow_faces && bgfx::isValid(program_))
    {
        viewport_ = {static_cast<float>(width), static_cast<float>(height), 1.0F / width,
                     1.0F / height};
        return true;
    }
    release();
    bgfx::VertexLayout layout;
    layout.begin().add(bgfx::Attrib::TexCoord0, 4, bgfx::AttribType::Float).end();
    lights_ = bgfx::createDynamicVertexBuffer(static_cast<std::uint32_t>(vectors), layout,
                                              BGFX_BUFFER_COMPUTE_READ);
    // Prepare one count word per cluster before the view's actual lights are available.
    clusters_ = bgfx::createDynamicIndexBuffer(
        static_cast<std::uint32_t>(count), BGFX_BUFFER_INDEX32 | BGFX_BUFFER_COMPUTE_READ_WRITE);
    shader_ = bgfx::createShader(bgfx::copy(cs_cluster_cull, sizeof(cs_cluster_cull)));
    if (bgfx::isValid(shader_))
    {
        program_ = bgfx::createProgram(shader_, false);
    }
    vsm_layout_ = bgfx::createUniform("u_vsm_layout", bgfx::UniformType::Vec4);
    if (vsm_vectors != 0)
    {
        vsm_view_uniform_ = bgfx::createUniform("u_vsm_view", bgfx::UniformFreq::View,
                                                bgfx::UniformType::Vec4, vsm_view_vectors);
    }
    grid_uniform_ =
        bgfx::createUniform("u_cluster_grid", bgfx::UniformFreq::View, bgfx::UniformType::Vec4);
    camera_uniform_ =
        bgfx::createUniform("u_cluster_camera", bgfx::UniformFreq::View, bgfx::UniformType::Vec4);
    counts_uniform_ =
        bgfx::createUniform("u_cluster_counts", bgfx::UniformFreq::View, bgfx::UniformType::Vec4);
    viewport_uniform_ =
        bgfx::createUniform("u_cluster_viewport", bgfx::UniformFreq::View, bgfx::UniformType::Vec4);
    view_uniform_ = bgfx::createUniform("u_cluster_view", bgfx::UniformType::Mat4);
    inverse_projection_uniform_ =
        bgfx::createUniform("u_cluster_inverse_projection", bgfx::UniformType::Mat4);
    if (!bgfx::isValid(vsm_layout_) || !bgfx::isValid(lights_) || !bgfx::isValid(clusters_) ||
        !bgfx::isValid(program_) || !bgfx::isValid(grid_uniform_) ||
        !bgfx::isValid(camera_uniform_) || !bgfx::isValid(counts_uniform_) ||
        !bgfx::isValid(viewport_uniform_) || !bgfx::isValid(view_uniform_) ||
        !bgfx::isValid(inverse_projection_uniform_) ||
        (vsm_vectors != 0 && !bgfx::isValid(vsm_view_uniform_)))
    {
        release();
        return false;
    }
    grid_ = grid;
    viewport_ = {static_cast<float>(width), static_cast<float>(height), 1.0F / width,
                 1.0F / height};
    cluster_count_ = static_cast<std::uint32_t>(count);
    cluster_buffer_words_ = cluster_count_;
    maximum_lights_ = config.maximum_lights;
    vsm_vectors_ = vsm_vectors;
    maximum_cluster_lights_ = config.maximum_cluster_lights;
    maximum_shadow_faces_ = config.maximum_local_shadow_faces;
    return true;
}

bool ClusteredLightingPass::cull(bgfx::ViewId view, const CameraSettings& camera,
                                 const float* view_matrix, const float* projection,
                                 std::span<const ClusterLight> lights,
                                 std::uint32_t directional_count,
                                 std::span<const ShadowRecord> shadows, const VsmPageBatch* vsm)
{
    ENGINE_PROFILE_ZONE("Render.clusterSubmit");
    ready_ = false;
    if (!bgfx::isValid(program_) || view_matrix == nullptr || projection == nullptr ||
        !finite_matrix(view_matrix) || !finite_matrix(projection) ||
        !std::isfinite(camera.near_clip) || !std::isfinite(camera.far_clip) ||
        camera.near_clip <= 0.0F || camera.far_clip <= camera.near_clip ||
        directional_count > lights.size() || lights.size() > maximum_lights_ ||
        shadows.size() > maximum_shadow_faces_)
    {
        return false;
    }
    float inverse[16]{};
    bx::mtxInverse(inverse, projection);
    if (!finite_matrix(inverse))
    {
        return false;
    }
    if (vsm && (vsm->page_table.empty() || vsm->page_table.size() > vsm_vectors_))
    {
        return false;
    }
    const auto local_count = lights.size() - directional_count;
    // A one-light cluster is either empty or contains the entire local list.
    // Its count word can use the existing full-list marker without storing an index.
    const auto capacity =
        local_count <= 1 ? 0 : std::min(local_count, std::size_t{maximum_cluster_lights_});
    // The configured upper bound on this product was checked during prepare().
    const auto cluster_words =
        static_cast<std::uint32_t>(cluster_count_ * (1 + (capacity + 1) / 2));
    if (cluster_words > cluster_buffer_words_)
    {
        const auto replacement = bgfx::createDynamicIndexBuffer(
            cluster_words, BGFX_BUFFER_INDEX32 | BGFX_BUFFER_COMPUTE_READ_WRITE);
        if (!bgfx::isValid(replacement))
        {
            return false;
        }
        bgfx::destroy(clusters_);
        clusters_ = replacement;
        cluster_buffer_words_ = cluster_words;
    }
    const auto vsm_bytes = vsm ? vsm->page_table.size() * sizeof(std::array<float, 4>) : 0;
    const auto light_bytes = lights.size_bytes();
    const auto shadow_bytes = shadows.size_bytes();
    const auto payload_bytes = light_bytes + shadow_bytes + vsm_bytes;
    const auto* upload = bgfx::alloc(
        static_cast<std::uint32_t>(std::max<std::size_t>(16, payload_bytes)));
    if (payload_bytes == 0)
    {
        // The empty scene still supplies a valid buffer binding.
        std::memset(upload->data, 0, upload->size);
    }
    if (light_bytes != 0)
    {
        std::memcpy(upload->data, lights.data(), light_bytes);
    }
    if (shadow_bytes != 0)
    {
        std::memcpy(upload->data + light_bytes, shadows.data(), shadow_bytes);
    }
    vsm_parameters_ = {float((light_bytes + shadow_bytes) / 16), vsm ? 1.F : 0.F,
                       vsm && vsm->diagnostics ? float(vsm->debug_options.mode) + 1.F : 0.F, 0.F};
    if (vsm_bytes)
    {
        vsm_view_ = vsm_pack_view(vsm->view, vsm->dominant_level);
        std::memcpy(upload->data + light_bytes + shadow_bytes, vsm->page_table.data(), vsm_bytes);
    }
    bgfx::update(lights_, 0, upload);
    const auto* caps = bgfx::getCaps();
    camera_ = {camera.near_clip, camera.far_clip, caps->homogeneousDepth ? 1.0F : 0.0F,
               caps->originBottomLeft ? 1.0F : 0.0F};
    counts_ = {static_cast<float>(lights.size()), static_cast<float>(capacity),
               static_cast<float>(lights.size() * 4), static_cast<float>(directional_count)};
    ENGINE_PROFILE_PLOT("Render.clusterIndexBytes",
                        static_cast<std::int64_t>(cluster_buffer_words_ * sizeof(std::uint32_t)));
    bgfx::setViewName(view, "Clustered lighting");
    bgfx::setViewMode(view, bgfx::ViewMode::Sequential);
    bgfx::setViewUniform(view, grid_uniform_, grid_.data());
    bgfx::setViewUniform(view, camera_uniform_, camera_.data());
    bgfx::setViewUniform(view, counts_uniform_, counts_.data());
    bgfx::setViewUniform(view, viewport_uniform_, viewport_.data());
    bgfx::setUniform(view_uniform_, view_matrix);
    bgfx::setUniform(inverse_projection_uniform_, inverse);
    bgfx::setBuffer(0, lights_, bgfx::Access::Read);
    bgfx::setBuffer(1, clusters_, bgfx::Access::Write);
    bgfx::dispatch(view, program_, (cluster_count_ + 63) / 64);
    ready_ = true;
    return true;
}

void ClusteredLightingPass::bind_view(bgfx::ViewId view, bool virtual_shadow) const
{
    if (!ready_)
    {
        return;
    }
    bgfx::setViewUniform(view, grid_uniform_, grid_.data());
    bgfx::setViewUniform(view, camera_uniform_, camera_.data());
    bgfx::setViewUniform(view, counts_uniform_, counts_.data());
    bgfx::setViewUniform(view, viewport_uniform_, viewport_.data());
    if (virtual_shadow && vsm_parameters_[1] > 0.5F)
    {
        bgfx::setViewUniform(view, vsm_view_uniform_, vsm_view_.data(), vsm_view_vectors);
    }
}

void ClusteredLightingPass::bind(bool virtual_shadow) const
{
    if (!ready_)
    {
        return;
    }
    auto layout = vsm_parameters_;
    if (!virtual_shadow)
    {
        layout[1] = layout[2] = 0;
    }
    bgfx::setUniform(vsm_layout_, layout.data());
    bgfx::setBuffer(5, lights_, bgfx::Access::Read);
    bgfx::setBuffer(7, clusters_, bgfx::Access::Read);
}

void ClusteredLightingPass::release() noexcept
{
    if (bgfx::isValid(lights_))
    {
        bgfx::destroy(lights_);
    }
    if (bgfx::isValid(clusters_))
    {
        bgfx::destroy(clusters_);
    }
    if (bgfx::isValid(program_))
    {
        bgfx::destroy(program_);
    }
    if (bgfx::isValid(shader_))
    {
        bgfx::destroy(shader_);
    }
    for (auto uniform :
         {grid_uniform_, camera_uniform_, counts_uniform_, viewport_uniform_, view_uniform_,
          inverse_projection_uniform_, vsm_layout_, vsm_view_uniform_})
    {
        if (bgfx::isValid(uniform))
        {
            bgfx::destroy(uniform);
        }
    }
    lights_ = BGFX_INVALID_HANDLE;
    clusters_ = BGFX_INVALID_HANDLE;
    program_ = BGFX_INVALID_HANDLE;
    shader_ = BGFX_INVALID_HANDLE;
    grid_uniform_ = camera_uniform_ = counts_uniform_ = view_uniform_ =
        inverse_projection_uniform_ = BGFX_INVALID_HANDLE;
    viewport_uniform_ = vsm_layout_ = vsm_view_uniform_ = BGFX_INVALID_HANDLE;
    vsm_vectors_ = 0;
    vsm_parameters_ = {};
    grid_ = {};
    cluster_count_ = cluster_buffer_words_ = maximum_lights_ = 0;
    maximum_cluster_lights_ = maximum_shadow_faces_ = 0;
    ready_ = false;
}

std::uint32_t ClusteredLightingPass::resource_count() const noexcept
{
    return static_cast<std::uint32_t>(bgfx::isValid(lights_)) +
           static_cast<std::uint32_t>(bgfx::isValid(clusters_)) +
           static_cast<std::uint32_t>(bgfx::isValid(program_)) +
           static_cast<std::uint32_t>(bgfx::isValid(shader_)) +
           static_cast<std::uint32_t>(bgfx::isValid(grid_uniform_)) +
           static_cast<std::uint32_t>(bgfx::isValid(camera_uniform_)) +
           static_cast<std::uint32_t>(bgfx::isValid(counts_uniform_)) +
           static_cast<std::uint32_t>(bgfx::isValid(viewport_uniform_)) +
           static_cast<std::uint32_t>(bgfx::isValid(vsm_layout_)) +
           static_cast<std::uint32_t>(bgfx::isValid(vsm_view_uniform_)) +
           static_cast<std::uint32_t>(bgfx::isValid(view_uniform_)) +
           static_cast<std::uint32_t>(bgfx::isValid(inverse_projection_uniform_));
}

} // namespace engine::render_detail
