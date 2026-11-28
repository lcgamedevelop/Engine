#include "runtime/render/passes/skinning_pass.hpp"

#include "runtime/performance/profiling.hpp"
#include "runtime/render/render_constants.hpp"
#include "runtime/render/resources/gpu_geometry.hpp"

#include <SDL3/SDL.h>

#include "cs_skinning.bin.h"

#include <algorithm>

namespace engine::render_detail
{

SkinningPass::~SkinningPass() noexcept
{
    ENGINE_PROFILE_FUNCTION();
    release();
}

bool SkinningPass::prepare()
{
    ENGINE_PROFILE_FUNCTION();
    if (bgfx::isValid(program_))
    {
        return true;
    }
    const auto* caps = bgfx::getCaps();
    if (caps == nullptr || (caps->supported & BGFX_CAPS_COMPUTE) == 0 ||
        caps->limits.maxComputeBindings < 4)
    {
        SDL_LogError(SDL_LOG_CATEGORY_RENDER,
                     "GPU skinning requires compute shaders and four buffer bindings");
        return false;
    }
    release();
    shader_ = bgfx::createShader(bgfx::copy(cs_skinning, sizeof(cs_skinning)));
    if (!bgfx::isValid(shader_))
    {
        return false;
    }
    program_ = bgfx::createProgram(shader_, false);
    parameters_ = bgfx::createUniform("u_skinning_parameters", bgfx::UniformType::Vec4);
    if (!bgfx::isValid(program_) || !bgfx::isValid(parameters_))
    {
        release();
        return false;
    }
    bgfx::setViewName(skinning_view, "Skinning");
    bgfx::setViewMode(skinning_view, bgfx::ViewMode::Sequential);
    return true;
}

bool SkinningPass::deform(const GpuGeometry& geometry,
                          std::span<const std::array<float, 16>> matrices,
                          bgfx::DynamicVertexBufferHandle& palette,
                          bgfx::DynamicVertexBufferHandle& output)
{
    enum Stage : FramePassId
    {
        upload_palette,
        ensure_output,
        deform_vertices
    };
    enum Resource : FrameResourceId
    {
        matrices_cpu,
        source_vertices,
        influences,
        layout,
        gpu_palette,
        buffer,
        vertices
    };
    static constexpr FrameResourceId imports[]{matrices_cpu, source_vertices, influences, layout};
    static constexpr FrameResourceId palette_input[]{matrices_cpu}, palette_output[]{gpu_palette};
    static constexpr FrameResourceId layout_input[]{layout}, allocated[]{buffer};
    static constexpr FrameResourceId compute_inputs[]{source_vertices, influences, gpu_palette,
                                                      buffer};
    static constexpr FrameResourceId roots[]{vertices};
    static constexpr FramePassDeclaration passes[]{{upload_palette, palette_input, palette_output},
                                                   {ensure_output, layout_input, allocated},
                                                   {deform_vertices, compute_inputs, roots}};
    FramePlanError error;
    if (matrices.empty() || matrices.size() % 2 != 0 || matrices.size() / 2 > 65536 ||
        !(compute_plan_.passes().empty() ? compute_plan_.compile(passes, imports, roots, error)
                                         : compute_plan_.begin_cached_frame()))
    {
        return false;
    }
    return compute_plan_.perform(
        [&](FramePassId pass)
        {
            if (pass == upload_palette)
            {
                const auto* memory =
                    bgfx::copy(matrices.data(), static_cast<std::uint32_t>(matrices.size_bytes()));
                if (bgfx::isValid(palette))
                {
                    bgfx::update(palette, 0, memory);
                }
                else
                {
                    palette = bgfx::createDynamicVertexBuffer(memory, GpuGeometry::vector_layout(),
                                                              BGFX_BUFFER_COMPUTE_READ);
                }
                return bgfx::isValid(palette);
            }
            if (pass == ensure_output)
            {
                if (!bgfx::isValid(output))
                {
                    output = bgfx::createDynamicVertexBuffer(geometry.vertex_count,
                                                             GpuGeometry::vertex_layout(),
                                                             BGFX_BUFFER_COMPUTE_READ_WRITE);
                }
                return bgfx::isValid(output);
            }
            return dispatch(geometry, palette, output,
                            static_cast<std::uint32_t>(matrices.size() / 2));
        },
        error);
}

bool SkinningPass::upload_vertices(std::span<const GpuVertex> vertices,
                                   bgfx::DynamicVertexBufferHandle& output)
{
    static constexpr FrameResourceId packed[]{0}, uploaded[]{1};
    static constexpr FramePassDeclaration passes[]{{0, packed, uploaded}};
    FramePlanError error;
    if (vertices.empty() || vertices.size_bytes() > UINT32_MAX ||
        !(upload_plan_.passes().empty() ? upload_plan_.compile(passes, packed, uploaded, error)
                                        : upload_plan_.begin_cached_frame()))
    {
        return false;
    }
    // bgfx examples/35-dynamic: preserve the topology and update only the vertex stream.
    // Copyright 2011-2025 Branimir Karadzic; third_party/notices/bgfx.txt.
    return upload_plan_.perform(
        [&](FramePassId)
        {
            const auto* memory =
                bgfx::copy(vertices.data(), static_cast<std::uint32_t>(vertices.size_bytes()));
            if (bgfx::isValid(output))
            {
                bgfx::update(output, 0, memory);
            }
            else
            {
                output = bgfx::createDynamicVertexBuffer(memory, GpuGeometry::vertex_layout(),
                                                         BGFX_BUFFER_COMPUTE_READ);
            }
            return bgfx::isValid(output);
        },
        error);
}

bool SkinningPass::dispatch(const GpuGeometry& geometry, bgfx::DynamicVertexBufferHandle palette,
                            bgfx::DynamicVertexBufferHandle output, std::uint32_t joint_count)
{
    ENGINE_PROFILE_ZONE("Render.skinningSubmit");
    if (!bgfx::isValid(program_) || !bgfx::isValid(geometry.vertices) ||
        !bgfx::isValid(geometry.influences) || !bgfx::isValid(palette) || !bgfx::isValid(output) ||
        geometry.vertex_count == 0 || joint_count == 0 || joint_count > 65536)
    {
        return false;
    }
    // bgfx examples/24-nbody: compute buffers become graphics inputs in a later view.
    // Copyright 2011-2025 Branimir Karadzic, BSD-2-Clause; third_party/notices/bgfx.txt.
    constexpr std::uint32_t group_size = 64;
    constexpr std::uint32_t maximum_groups = 65535;
    const auto groups = (geometry.vertex_count + group_size - 1) / group_size;
    const auto groups_x = std::min(groups, maximum_groups);
    const auto groups_y = (groups + groups_x - 1) / groups_x;
    if (groups_y > maximum_groups)
    {
        return false;
    }
    // 用两个精确的 16 位整数传递顶点数，避免 float 无法表达大于 2^24 的整数。
    const float parameters[] = {static_cast<float>(geometry.vertex_count & 65535U),
                                static_cast<float>(geometry.vertex_count >> 16U),
                                static_cast<float>(joint_count),
                                static_cast<float>(groups_x * group_size)};
    bgfx::setUniform(parameters_, parameters);
    bgfx::setBuffer(0, geometry.vertices, bgfx::Access::Read);
    bgfx::setBuffer(1, geometry.influences, bgfx::Access::Read);
    bgfx::setBuffer(2, palette, bgfx::Access::Read);
    bgfx::setBuffer(3, output, bgfx::Access::Write);
    bgfx::dispatch(skinning_view, program_, groups_x, groups_y, 1);
    return true;
}

std::uint32_t SkinningPass::resource_count() const noexcept
{
    ENGINE_PROFILE_FUNCTION();
    return static_cast<std::uint32_t>(bgfx::isValid(shader_)) +
           static_cast<std::uint32_t>(bgfx::isValid(program_)) +
           static_cast<std::uint32_t>(bgfx::isValid(parameters_));
}

void SkinningPass::release() noexcept
{
    ENGINE_PROFILE_FUNCTION();
    if (bgfx::isValid(program_))
    {
        bgfx::destroy(program_);
        program_ = BGFX_INVALID_HANDLE;
    }
    if (bgfx::isValid(shader_))
    {
        bgfx::destroy(shader_);
        shader_ = BGFX_INVALID_HANDLE;
    }
    if (bgfx::isValid(parameters_))
    {
        bgfx::destroy(parameters_);
        parameters_ = BGFX_INVALID_HANDLE;
    }
}

} // namespace engine::render_detail
