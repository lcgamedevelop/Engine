#include "runtime/render/passes/debug_draw_pass.hpp"

#include "runtime/render/render_color.hpp"
#include "runtime/render/render_constants.hpp"
#include "runtime/render/render_math.hpp"
#include "runtime/render/render_submission.hpp"
#include "runtime/render/resources/gpu_mesh.hpp"
#include "runtime/render/resources/gpu_program.hpp"
#include "runtime/render/resources/gpu_vertex.hpp"
#include "runtime/resource/resource.hpp"
#include "runtime/scene/camera_math.hpp"
#include "runtime/scene/ecs_scene.hpp"
#include "runtime/scene/ecs_storage.hpp"
#include "runtime/scene/mesh_renderer.hpp"
#include "runtime/scene/scene.hpp"
#include "runtime/scene/transform.hpp"
#include "runtime/world/world_system.hpp"

#include <bx/bounds.h>
#include <bx/math.h>
#include <debugdraw/debugdraw.h>

#include "fs_debug_wireframe.bin.h"
#include "vs_debug_meshlets.bin.h"
#include "vs_debug_meshlets_instanced.bin.h"
#include "vs_debug_wireframe.bin.h"

#include <algorithm>
#include <bit>
#include <limits>

namespace engine::render_detail
{
namespace
{
constexpr auto feature_count = static_cast<std::size_t>(RenderFeature::count);

std::uint32_t linear_abgr(std::uint32_t rgba)
{
    const auto channel = [](std::uint32_t value)
    { return static_cast<std::uint32_t>(255.0F * display_srgb_to_linear(value / 255.0F) + 0.5F); };
    return ((rgba & 255U) << 24U) | (channel((rgba >> 8U) & 255U) << 16U) |
           (channel((rgba >> 16U) & 255U) << 8U) | channel(rgba >> 24U);
}
} // namespace

DebugDrawPass::~DebugDrawPass() noexcept
{
    release();
}

bool DebugDrawPass::supports_wireframe() noexcept
{
    const auto* caps = bgfx::getCaps();
    return caps != nullptr && (caps->supported & BGFX_CAPS_COMPUTE) != 0 &&
           caps->limits.maxComputeBindings >= 2;
}

bool DebugDrawPass::supports_meshlet_colors() noexcept
{
    return supports_wireframe() && bgfx::getCaps()->limits.maxComputeBindings >= 4 &&
           bgfx::getCaps()->limits.maxInstanceData >= 4;
}

bool DebugDrawPass::prepare(bool wireframe, bool meshlet_colors)
{
    if (!initialized_)
    {
        // Upstream 29-debugdraw owns its shared geometry and shader resources.
        ddInit();
        initialized_ = true;
    }
    const auto create_program = [](const std::uint8_t* code, std::uint32_t size)
    {
        const auto vertex = bgfx::createShader(bgfx::copy(code, size));
        const auto fragment =
            bgfx::createShader(bgfx::copy(fs_debug_wireframe, sizeof(fs_debug_wireframe)));
        bgfx::ProgramHandle program = BGFX_INVALID_HANDLE;
        if (bgfx::isValid(vertex) && bgfx::isValid(fragment))
        {
            program = bgfx::createProgram(vertex, fragment, false);
        }
        if (bgfx::isValid(vertex))
        {
            bgfx::destroy(vertex);
        }
        if (bgfx::isValid(fragment))
        {
            bgfx::destroy(fragment);
        }
        return program;
    };
    if (wireframe && !bgfx::isValid(wireframe_))
    {
        wireframe_ = create_program(vs_debug_wireframe, sizeof(vs_debug_wireframe));
        parameters_ = bgfx::createUniform("u_debug_wire_parameters", bgfx::UniformType::Vec4);
        color_ = bgfx::createUniform("u_debug_wire_color", bgfx::UniformType::Vec4);
    }
    if (meshlet_colors && !bgfx::isValid(meshlet_colors_))
    {
        meshlet_colors_ = create_program(vs_debug_meshlets, sizeof(vs_debug_meshlets));
        meshlet_colors_instanced_ =
            create_program(vs_debug_meshlets_instanced, sizeof(vs_debug_meshlets_instanced));
        meshlet_parameters_ =
            bgfx::createUniform("u_debug_meshlet_parameters", bgfx::UniformType::Vec4, 2);
    }
    if ((wireframe &&
         (!bgfx::isValid(wireframe_) || !bgfx::isValid(parameters_) || !bgfx::isValid(color_))) ||
        (meshlet_colors &&
         (!bgfx::isValid(meshlet_colors_) || !bgfx::isValid(meshlet_colors_instanced_) ||
          !bgfx::isValid(meshlet_parameters_))))
    {
        release();
        return false;
    }
    return true;
}

bool DebugDrawPass::draw(const WorldSystem& world, Camera camera, std::span<const GpuMesh> meshes,
                         bgfx::FrameBufferHandle framebuffer, std::uint16_t width,
                         std::uint16_t height, const float* view, const float* projection,
                         const std::array<bool, feature_count>& features,
                         std::span<const DebugLine> lines, std::span<const std::size_t> lod_levels,
                         std::span<const DebugLine> meshlet_lines, const RenderFrameData& frame,
                         const ViewDrawList& draws)
{
    const auto enabled = [&](RenderFeature feature)
    { return features[static_cast<std::size_t>(feature)]; };
    const bool wireframe = enabled(RenderFeature::wireframe) && supports_wireframe();
    const bool meshlet_colors = enabled(RenderFeature::meshlet_colors) && supports_meshlet_colors();
    if (!meshlet_colors && bgfx::isValid(meshlet_instances_))
    {
        bgfx::destroy(meshlet_instances_);
        meshlet_instances_ = BGFX_INVALID_HANDLE;
        meshlet_instance_capacity_ = 0;
    }
    if (!wireframe && !meshlet_colors && !enabled(RenderFeature::mesh_bounds) &&
        !enabled(RenderFeature::camera_frusta) &&
        (!enabled(RenderFeature::debug_draw) || lines.empty()) &&
        (!enabled(RenderFeature::meshlet_bounds) || meshlet_lines.empty()))
    {
        return true;
    }
    if (!prepare(wireframe, meshlet_colors))
    {
        return false;
    }
    if (meshlet_colors)
    {
        if (std::any_of(draws.opaque.begin(), draws.opaque.end(),
                        [](const auto& item) { return item.instanced_transform; }))
        {
            constexpr auto stride = sizeof(std::array<float, 16>);
            if (frame.meshes.size() > UINT32_MAX / stride)
            {
                return false;
            }
            const auto count = static_cast<std::uint32_t>(frame.meshes.size());
            if (count > meshlet_instance_capacity_)
            {
                if (bgfx::isValid(meshlet_instances_))
                {
                    bgfx::destroy(meshlet_instances_);
                }
                bgfx::VertexLayout layout;
                layout.begin();
                for (const auto attribute : {bgfx::Attrib::TexCoord0, bgfx::Attrib::TexCoord1,
                                             bgfx::Attrib::TexCoord2, bgfx::Attrib::TexCoord3})
                {
                    layout.add(attribute, 4, bgfx::AttribType::Float);
                }
                layout.end();
                meshlet_instances_ = bgfx::createDynamicVertexBuffer(count, layout);
                meshlet_instance_capacity_ = bgfx::isValid(meshlet_instances_) ? count : 0;
                if (!bgfx::isValid(meshlet_instances_))
                {
                    return false;
                }
            }
            std::vector<std::array<float, 16>> matrices;
            matrices.reserve(count);
            for (const auto& mesh : frame.meshes)
            {
                matrices.push_back(mesh.model);
            }
            bgfx::update(meshlet_instances_, 0,
                         bgfx::copy(matrices.data(), static_cast<std::uint32_t>(count * stride)));
        }
        bgfx::setViewName(meshlet_color_view, "Meshlet colors");
        bgfx::setViewFrameBuffer(meshlet_color_view, framebuffer);
        bgfx::setViewRect(meshlet_color_view, 0, 0, width, height);
        bgfx::setViewClear(meshlet_color_view, BGFX_CLEAR_NONE);
        bgfx::setViewMode(meshlet_color_view, bgfx::ViewMode::Sequential);
        bgfx::setViewTransform(meshlet_color_view, view, projection);
        for (const auto& item : draws.opaque)
        {
            const auto& mesh = frame.meshes[item.mesh];
            const auto& material = mesh.gpu->materials[item.slot];
            if (!mesh.instance_eligible || mesh.plane || material.frame_textures ||
                !material.program || !material.program->definition.meshlet_culling_compatible ||
                item.geometry != mesh.gpu->geometry.get())
            {
                continue;
            }
            const auto* data = item.lod == 0 ? mesh.data->meshlets.get()
                                             : mesh.data->lods[item.lod - 1]->meshlets.get();
            const auto indices = item.geometry->meshlet_indices(item.lod);
            const auto descriptors = item.geometry->meshlet_descriptors(item.lod);
            if (!data || !bgfx::isValid(indices) || !bgfx::isValid(descriptors))
            {
                continue;
            }
            std::size_t part = 0;
            const auto ranges = item.geometry->submeshes(item.lod);
            if (!ranges.empty())
            {
                const auto found = std::find_if(ranges.begin(), ranges.end(),
                                                [&](const auto& range)
                                                {
                                                    return range.first_index() == item.first &&
                                                           range.index_count() == item.count;
                                                });
                if (found == ranges.end())
                {
                    return false;
                }
                part = static_cast<std::size_t>(found - ranges.begin());
            }
            if (part >= data->submeshes.size())
            {
                return false;
            }
            const auto& range = data->submeshes[part];
            if (range.meshlet_count() == 0)
            {
                continue;
            }
            std::uint32_t hash = 2166136261U;
            for (const auto c : mesh.data->path)
            {
                hash = (hash ^ static_cast<unsigned char>(c)) * 16777619U;
            }
            const std::array<float, 8> parameters{
                std::bit_cast<float>(data->meshlets[range.first_meshlet()].first_index()),
                std::bit_cast<float>(range.first_meshlet()),
                std::bit_cast<float>(range.meshlet_count()),
                std::bit_cast<float>(hash),
                static_cast<float>(sizeof(GpuVertex) / sizeof(float)),
                static_cast<float>(item.lod),
                0,
                0};
            bgfx::setUniform(meshlet_parameters_, parameters.data(), 2);
            bgfx::setTransform(mesh.model.data());
            // Slot 0 is reserved for the instance stream on Metal.
            bgfx::setBuffer(1, item.geometry->vertices, bgfx::Access::Read);
            bgfx::setBuffer(2, indices, bgfx::Access::Read);
            bgfx::setBuffer(3, descriptors, bgfx::Access::Read);
            bgfx::setVertexCount(item.count);
            if (item.instanced_transform)
            {
                bgfx::setInstanceDataBuffer(meshlet_instances_,
                                            static_cast<std::uint32_t>(item.mesh), 1);
            }
            // Only replace the existing opaque surface; preserve cutout holes and depth.
            bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_DEPTH_TEST_EQUAL | BGFX_STATE_MSAA |
                           cull_state(material.cull, mesh.model.data()));
            bgfx::submit(meshlet_color_view,
                         item.instanced_transform ? meshlet_colors_instanced_ : meshlet_colors_);
            submitted_[static_cast<std::size_t>(RenderFeature::meshlet_colors)] = true;
        }
    }
    bgfx::setViewName(debug_view, "Scene diagnostics");
    bgfx::setViewFrameBuffer(debug_view, framebuffer);
    bgfx::setViewRect(debug_view, 0, 0, width, height);
    bgfx::setViewClear(debug_view, BGFX_CLEAR_NONE);
    bgfx::setViewTransform(debug_view, view, projection);
    bgfx::setViewMode(debug_view, bgfx::ViewMode::Sequential);
    DebugDrawEncoder encoder;
    encoder.begin(debug_view);
    encoder.setWireframe(true);
    encoder.setState(true, false, false);
    for (const auto& mesh : meshes)
    {
        const auto node = world.find_node(mesh.node_id);
        const auto renderer = node.get_component<MeshRenderer>();
        const auto transform = node.get_component<Transform>();
        std::array<float, 16> matrix{};
        if (!renderer || renderer.instance_id() != mesh.instance_id || !renderer.active() ||
            !renderer.visible_in_hierarchy() || !camera.mesh_visible(mesh.instance_id) ||
            !transform || !transform.world_matrix(matrix.data()) || !mesh.geometry)
        {
            continue;
        }
        if (enabled(RenderFeature::mesh_bounds))
        {
            encoder.setColor(linear_abgr(0x50ff90ff));
            encoder.setTransform(matrix.data());
            encoder.draw(bx::Aabb{{mesh.minimum[0], mesh.minimum[1], mesh.minimum[2]},
                                  {mesh.maximum[0], mesh.maximum[1], mesh.maximum[2]}});
            submitted_[static_cast<std::size_t>(RenderFeature::mesh_bounds)] = true;
        }
        if (wireframe)
        {
            const auto offset = static_cast<std::size_t>(&mesh - meshes.data());
            const auto level = offset < lod_levels.size() ? lod_levels[offset] : 0;
            const auto count = mesh.geometry->index_count(level);
            const auto indices = mesh.geometry->index_buffer(level);
            if (!bgfx::isValid(indices) || count == 0 ||
                count > std::numeric_limits<std::uint32_t>::max() / 6U)
            {
                continue;
            }
            const float parameters[] = {static_cast<float>(sizeof(GpuVertex) / sizeof(float)), 1.1F,
                                        0, 0};
            const float color[] = {0.1F, 0.8F, 1.0F, 0.9F};
            bgfx::setUniform(parameters_, parameters);
            bgfx::setUniform(color_, color);
            bgfx::setTransform(matrix.data());
            if (bgfx::isValid(mesh.deformed_vertices))
            {
                bgfx::setBuffer(0, mesh.deformed_vertices, bgfx::Access::Read);
            }
            else
            {
                bgfx::setBuffer(0, mesh.geometry->vertices, bgfx::Access::Read);
            }
            bgfx::setBuffer(1, indices, bgfx::Access::Read);
            bgfx::setVertexCount(count * 6U);
            bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_DEPTH_TEST_LEQUAL |
                           BGFX_STATE_BLEND_ALPHA | BGFX_STATE_MSAA);
            bgfx::submit(debug_view, wireframe_);
            submitted_[static_cast<std::size_t>(RenderFeature::wireframe)] = true;
        }
    }
    encoder.setTransform(nullptr);
    if (enabled(RenderFeature::camera_frusta))
    {
        auto query = flecs::world(world.native_world())
                         .query_builder<const ecs_detail::ComponentStorage<Camera::Data>>()
                         .with<ecs_detail::Published>()
                         .build();
        query.each(
            [&](flecs::entity entity, const auto&)
            {
                const auto node = world.find_node(entity.id());
                const auto target = node.get_component<Camera>();
                const auto transform = node.get_component<Transform>();
                float model[16]{}, observer[16]{}, lens[16]{}, combined[16]{};
                CameraSettings settings;
                if (target.get_settings(settings) && target.active() &&
                    target.visible_in_hierarchy() && transform && transform.world_matrix(model) &&
                    camera_detail::view_matrix(model, observer) &&
                    camera_detail::projection_matrix(settings, static_cast<double>(width) / height,
                                                     bgfx::getCaps()->homogeneousDepth, lens))
                {
                    bx::mtxMul(combined, observer, lens);
                    encoder.setColor(linear_abgr(0xffc050ff));
                    encoder.drawFrustum(combined);
                    submitted_[static_cast<std::size_t>(RenderFeature::camera_frusta)] = true;
                }
            });
    }
    if (enabled(RenderFeature::debug_draw))
    {
        for (const auto& line : lines)
        {
            encoder.setState(line.depth_test, false, false);
            encoder.setColor(linear_abgr(line.rgba));
            encoder.moveTo(line.start.x(), line.start.y(), line.start.z());
            encoder.lineTo(line.end.x(), line.end.y(), line.end.z());
        }
        submitted_[static_cast<std::size_t>(RenderFeature::debug_draw)] = !lines.empty();
    }
    if (enabled(RenderFeature::meshlet_bounds))
    {
        for (const auto& line : meshlet_lines)
        {
            encoder.setState(line.depth_test, false, false);
            encoder.setColor(linear_abgr(line.rgba));
            encoder.moveTo(line.start.x(), line.start.y(), line.start.z());
            encoder.lineTo(line.end.x(), line.end.y(), line.end.z());
        }
        submitted_[static_cast<std::size_t>(RenderFeature::meshlet_bounds)] =
            !meshlet_lines.empty();
    }
    encoder.end();
    return true;
}

void DebugDrawPass::reset_frame() noexcept
{
    submitted_.fill(false);
}

bool DebugDrawPass::submitted(RenderFeature feature) const noexcept
{
    const auto index = static_cast<std::size_t>(feature);
    return index < submitted_.size() && submitted_[index];
}

std::uint32_t DebugDrawPass::resource_count() const noexcept
{
    // Fixed upstream ddInit creates seven programs, two uniforms, one atlas and two buffers.
    // This integration never creates upstream sprites or custom geometry handles.
    return (initialized_ ? 12U : 0U) + static_cast<std::uint32_t>(bgfx::isValid(wireframe_)) +
           static_cast<std::uint32_t>(bgfx::isValid(parameters_)) +
           static_cast<std::uint32_t>(bgfx::isValid(color_)) +
           static_cast<std::uint32_t>(bgfx::isValid(meshlet_colors_)) +
           static_cast<std::uint32_t>(bgfx::isValid(meshlet_colors_instanced_)) +
           static_cast<std::uint32_t>(bgfx::isValid(meshlet_instances_)) +
           static_cast<std::uint32_t>(bgfx::isValid(meshlet_parameters_));
}

void DebugDrawPass::release() noexcept
{
    if (bgfx::isValid(wireframe_))
    {
        bgfx::destroy(wireframe_);
    }
    if (bgfx::isValid(parameters_))
    {
        bgfx::destroy(parameters_);
    }
    if (bgfx::isValid(color_))
    {
        bgfx::destroy(color_);
    }
    if (bgfx::isValid(meshlet_colors_))
    {
        bgfx::destroy(meshlet_colors_);
    }
    if (bgfx::isValid(meshlet_colors_instanced_))
    {
        bgfx::destroy(meshlet_colors_instanced_);
    }
    if (bgfx::isValid(meshlet_instances_))
    {
        bgfx::destroy(meshlet_instances_);
    }
    if (bgfx::isValid(meshlet_parameters_))
    {
        bgfx::destroy(meshlet_parameters_);
    }
    wireframe_ = BGFX_INVALID_HANDLE;
    parameters_ = BGFX_INVALID_HANDLE;
    color_ = BGFX_INVALID_HANDLE;
    meshlet_colors_ = BGFX_INVALID_HANDLE;
    meshlet_colors_instanced_ = BGFX_INVALID_HANDLE;
    meshlet_instances_ = BGFX_INVALID_HANDLE;
    meshlet_instance_capacity_ = 0;
    meshlet_parameters_ = BGFX_INVALID_HANDLE;
    if (initialized_)
    {
        ddShutdown();
    }
    initialized_ = false;
    reset_frame();
}
} // namespace engine::render_detail
