#include "runtime/render/resources/gpu_program.hpp"

#include "runtime/performance/function_profile.hpp"
#include "runtime/performance/profiling.hpp"
#include "runtime/render/render_constants.hpp"
#include "runtime/render/resources/gpu_texture.hpp"
#include "runtime/render/vsm/vsm_types.hpp"
#include "runtime/resource/resource.hpp"
#include "runtime/scene/material.hpp"

#include "fs_cube_depth.bin.h"
#include "fs_cube_pbr_opaque.bin.h"
#include "fs_shadow_solid.bin.h"
#include "fs_terrain_depth.bin.h"
#include "vs_shadow_instanced.bin.h"
#include "vs_shadow_position.bin.h"

#include <SDL3/SDL_log.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>
#include <utility>

namespace engine::render_detail
{

GpuProgram::~GpuProgram() noexcept
{
    ENGINE_PROFILE_FUNCTION();
    release();
}

GpuProgram::GpuProgram(GpuProgram&& other) noexcept
    : path(std::move(other.path)), resource(std::move(other.resource)),
      definition(std::move(other.definition)),
      vertex_shader(std::exchange(other.vertex_shader, bgfx::ShaderHandle{bgfx::kInvalidHandle})),
      fragment_shader(
          std::exchange(other.fragment_shader, bgfx::ShaderHandle{bgfx::kInvalidHandle})),
      program(std::exchange(other.program, bgfx::ProgramHandle{bgfx::kInvalidHandle})),
      pbr_opaque_shader(
          std::exchange(other.pbr_opaque_shader, bgfx::ShaderHandle{bgfx::kInvalidHandle})),
      pbr_opaque_program(
          std::exchange(other.pbr_opaque_program, bgfx::ProgramHandle{bgfx::kInvalidHandle})),
      instanced_pbr_opaque_program(std::exchange(other.instanced_pbr_opaque_program,
                                                 bgfx::ProgramHandle{bgfx::kInvalidHandle})),
      surface_shader(std::exchange(other.surface_shader, bgfx::ShaderHandle{bgfx::kInvalidHandle})),
      surface_program(
          std::exchange(other.surface_program, bgfx::ProgramHandle{bgfx::kInvalidHandle})),
      depth_shader(std::exchange(other.depth_shader, bgfx::ShaderHandle{bgfx::kInvalidHandle})),
      depth_program(std::exchange(other.depth_program, bgfx::ProgramHandle{bgfx::kInvalidHandle})),
      instanced_depth_program(
          std::exchange(other.instanced_depth_program, bgfx::ProgramHandle{bgfx::kInvalidHandle})),
      solid_depth_program(
          std::exchange(other.solid_depth_program, bgfx::ProgramHandle{bgfx::kInvalidHandle})),
      instanced_solid_depth_program(std::exchange(other.instanced_solid_depth_program,
                                                 bgfx::ProgramHandle{bgfx::kInvalidHandle})),
      rvt_shader(std::exchange(other.rvt_shader, bgfx::ShaderHandle{bgfx::kInvalidHandle})),
      rvt_program(std::exchange(other.rvt_program, bgfx::ProgramHandle{bgfx::kInvalidHandle})),
      instanced_vertex_shader(
          std::exchange(other.instanced_vertex_shader, bgfx::ShaderHandle{bgfx::kInvalidHandle})),
      instanced_program(
          std::exchange(other.instanced_program, bgfx::ProgramHandle{bgfx::kInvalidHandle})),
      instanced_surface_program(std::exchange(other.instanced_surface_program,
                                              bgfx::ProgramHandle{bgfx::kInvalidHandle})),
      uniforms(std::exchange(other.uniforms, {})), samplers(std::exchange(other.samplers, {}))
{
    ENGINE_PROFILE_FUNCTION();
}

GpuProgram& GpuProgram::operator=(GpuProgram&& other) noexcept
{
    ENGINE_PROFILE_FUNCTION();
    if (this != &other)
    {
        release();
        path = std::move(other.path);
        resource = std::move(other.resource);
        definition = std::move(other.definition);
        vertex_shader =
            std::exchange(other.vertex_shader, bgfx::ShaderHandle{bgfx::kInvalidHandle});
        fragment_shader =
            std::exchange(other.fragment_shader, bgfx::ShaderHandle{bgfx::kInvalidHandle});
        program = std::exchange(other.program, bgfx::ProgramHandle{bgfx::kInvalidHandle});
        pbr_opaque_shader =
            std::exchange(other.pbr_opaque_shader, bgfx::ShaderHandle{bgfx::kInvalidHandle});
        pbr_opaque_program =
            std::exchange(other.pbr_opaque_program, bgfx::ProgramHandle{bgfx::kInvalidHandle});
        instanced_pbr_opaque_program = std::exchange(other.instanced_pbr_opaque_program,
                                                    bgfx::ProgramHandle{bgfx::kInvalidHandle});
        surface_shader =
            std::exchange(other.surface_shader, bgfx::ShaderHandle{bgfx::kInvalidHandle});
        surface_program =
            std::exchange(other.surface_program, bgfx::ProgramHandle{bgfx::kInvalidHandle});
        depth_shader = std::exchange(other.depth_shader, bgfx::ShaderHandle{bgfx::kInvalidHandle});
        depth_program =
            std::exchange(other.depth_program, bgfx::ProgramHandle{bgfx::kInvalidHandle});
        instanced_depth_program =
            std::exchange(other.instanced_depth_program, bgfx::ProgramHandle{bgfx::kInvalidHandle});
        solid_depth_program =
            std::exchange(other.solid_depth_program, bgfx::ProgramHandle{bgfx::kInvalidHandle});
        instanced_solid_depth_program = std::exchange(other.instanced_solid_depth_program,
                                                      bgfx::ProgramHandle{bgfx::kInvalidHandle});
        rvt_shader = std::exchange(other.rvt_shader, bgfx::ShaderHandle{bgfx::kInvalidHandle});
        rvt_program = std::exchange(other.rvt_program, bgfx::ProgramHandle{bgfx::kInvalidHandle});
        instanced_vertex_shader =
            std::exchange(other.instanced_vertex_shader, bgfx::ShaderHandle{bgfx::kInvalidHandle});
        instanced_program =
            std::exchange(other.instanced_program, bgfx::ProgramHandle{bgfx::kInvalidHandle});
        instanced_surface_program = std::exchange(other.instanced_surface_program,
                                                  bgfx::ProgramHandle{bgfx::kInvalidHandle});
        uniforms = std::exchange(other.uniforms, {});
        samplers = std::exchange(other.samplers, {});
    }
    return *this;
}

bool GpuProgram::create(const schema::ShaderDataT& shader)
{
    ENGINE_PROFILE_ZONE("Render.programCreate");
    release();
    path = shader.path;
    definition = shader;
    vertex_shader = bgfx::createShader(bgfx::copy(
        shader.vertex_code.data(), static_cast<std::uint32_t>(shader.vertex_code.size())));
    fragment_shader = bgfx::createShader(bgfx::copy(
        shader.fragment_code.data(), static_cast<std::uint32_t>(shader.fragment_code.size())));
    if (!bgfx::isValid(vertex_shader) || !bgfx::isValid(fragment_shader))
    {
        return false;
    }
    program = bgfx::createProgram(vertex_shader, fragment_shader);
    if (!bgfx::isValid(program))
    {
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Cannot link material program: %s", path.c_str());
        return false;
    }
    if (!shader.surface_fragment_code.empty())
    {
        surface_shader = bgfx::createShader(
            bgfx::copy(shader.surface_fragment_code.data(),
                       static_cast<std::uint32_t>(shader.surface_fragment_code.size())));
        if (!bgfx::isValid(surface_shader))
        {
            return false;
        }
        surface_program = bgfx::createProgram(vertex_shader, surface_shader);
        if (!bgfx::isValid(surface_program))
        {
            SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Cannot link surface program: %s", path.c_str());
            return false;
        }
    }
    if (!shader.rvt_fragment_code.empty())
    {
        rvt_shader = bgfx::createShader(
            bgfx::copy(shader.rvt_fragment_code.data(),
                       static_cast<std::uint32_t>(shader.rvt_fragment_code.size())));
        if (!bgfx::isValid(rvt_shader))
        {
            return false;
        }
        rvt_program = bgfx::createProgram(vertex_shader, rvt_shader);
        if (!bgfx::isValid(rvt_program))
        {
            return false;
        }
    }
    if (!shader.instanced_vertex_code.empty())
    {
        instanced_vertex_shader = bgfx::createShader(
            bgfx::copy(shader.instanced_vertex_code.data(),
                       static_cast<std::uint32_t>(shader.instanced_vertex_code.size())));
        if (!bgfx::isValid(instanced_vertex_shader))
        {
            return false;
        }
        instanced_program = bgfx::createProgram(instanced_vertex_shader, fragment_shader);
        if (!bgfx::isValid(instanced_program))
        {
            return false;
        }
        if (bgfx::isValid(surface_shader))
        {
            instanced_surface_program =
                bgfx::createProgram(instanced_vertex_shader, surface_shader);
            if (!bgfx::isValid(instanced_surface_program))
            {
                return false;
            }
        }
    }
    const bool builtin_pbr = path == "builtin://shaders/pbr";
    if (builtin_pbr)
    {
        pbr_opaque_shader =
            bgfx::createShader(bgfx::copy(fs_cube_pbr_opaque, sizeof(fs_cube_pbr_opaque)));
        if (!bgfx::isValid(pbr_opaque_shader))
        {
            return false;
        }
        pbr_opaque_program = bgfx::createProgram(vertex_shader, pbr_opaque_shader);
        if (bgfx::isValid(instanced_vertex_shader))
        {
            instanced_pbr_opaque_program =
                bgfx::createProgram(instanced_vertex_shader, pbr_opaque_shader);
        }
        if (!bgfx::isValid(pbr_opaque_program) ||
            (bgfx::isValid(instanced_vertex_shader) &&
             !bgfx::isValid(instanced_pbr_opaque_program)))
        {
            SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Cannot link opaque PBR program: %s",
                         path.c_str());
            return false;
        }
        // These shaders use the same position transforms as the built-in PBR vertices.
        const auto solid_fragment =
            bgfx::createShader(bgfx::copy(fs_shadow_solid, sizeof(fs_shadow_solid)));
        if (!bgfx::isValid(solid_fragment))
        {
            return false;
        }
        const auto link_solid_depth = [&](const std::uint8_t* code, std::uint32_t bytes)
        {
            ENGINE_PROFILE_FUNCTION_PROBE();
            const auto vertex = bgfx::createShader(bgfx::copy(code, bytes));
            if (!bgfx::isValid(vertex))
            {
                return bgfx::ProgramHandle{bgfx::kInvalidHandle};
            }
            const auto linked = bgfx::createProgram(vertex, solid_fragment);
            bgfx::destroy(vertex);
            return linked;
        };
        solid_depth_program = link_solid_depth(vs_shadow_position, sizeof(vs_shadow_position));
        if (bgfx::isValid(instanced_vertex_shader))
        {
            instanced_solid_depth_program =
                link_solid_depth(vs_shadow_instanced, sizeof(vs_shadow_instanced));
        }
        bgfx::destroy(solid_fragment);
        if (!bgfx::isValid(solid_depth_program) ||
            (bgfx::isValid(instanced_vertex_shader) &&
             !bgfx::isValid(instanced_solid_depth_program)))
        {
            SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Cannot link solid depth program: %s",
                         path.c_str());
            return false;
        }
    }
    const bool terrain = path == "builtin://shaders/terrain-pbr";
    if (terrain || builtin_pbr || path == "builtin://shaders/unlit" ||
        path == "builtin://shaders/blinn-phong")
    {
        const auto* code = terrain ? fs_terrain_depth : fs_cube_depth;
        const auto size = terrain ? sizeof(fs_terrain_depth) : sizeof(fs_cube_depth);
        depth_shader = bgfx::createShader(bgfx::copy(code, static_cast<std::uint32_t>(size)));
        if (!bgfx::isValid(depth_shader))
        {
            return false;
        }
        depth_program = bgfx::createProgram(vertex_shader, depth_shader);
        if (bgfx::isValid(instanced_vertex_shader))
        {
            instanced_depth_program = bgfx::createProgram(instanced_vertex_shader, depth_shader);
        }
        if (!bgfx::isValid(depth_program) ||
            (bgfx::isValid(instanced_vertex_shader) && !bgfx::isValid(instanced_depth_program)))
        {
            SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Cannot link depth coverage program: %s",
                         path.c_str());
            return false;
        }
    }
    // 反射与定义双向匹配，避免遗漏参数继承上一对象的 uniform 值。
    std::vector<bgfx::UniformInfo> reflected;
    std::vector<std::string> opaque_inputs;
    for (auto stage : {vertex_shader, fragment_shader, surface_shader, instanced_vertex_shader,
                       rvt_shader, depth_shader, pbr_opaque_shader})
    {
        if (!bgfx::isValid(stage))
        {
            continue;
        }
        const bool opaque_stage = builtin_pbr &&
                                  (stage.idx == pbr_opaque_shader.idx ||
                                   stage.idx == vertex_shader.idx ||
                                   stage.idx == instanced_vertex_shader.idx);
        const auto count = bgfx::getShaderUniforms(stage);
        std::vector<bgfx::UniformHandle> handles(count);
        bgfx::getShaderUniforms(stage, handles.data(), count);
        for (auto handle : handles)
        {
            bgfx::UniformInfo info{};
            bgfx::getUniformInfo(handle, info);
            const std::string_view name(info.name);
            if (name == "u_normal_transform" || name == "u_camera_position" ||
                name == "u_light_direction" || name == "u_light_color" || name == "u_ambient" ||
                name == "u_tonemap" || name == "u_surface" || name == "u_ambient_sky" ||
                name == "u_ambient_equator" || name == "u_ambient_ground" ||
                name == "u_fog_color" || name == "u_fog_range" || name == "u_shadow_matrices" ||
                name == "u_shadow_rects" || name == "u_shadow_splits" ||
                name == "u_shadow_params" || name == "u_shadow_settings" ||
                name == "u_vsm_layout" || name == "u_vsm_view" || name == "s_virtual_shadow" ||
                name == "s_shadow_map" || name == "u_ibl_params" || name == "s_ibl_irradiance" ||
                name == "s_ibl_prefilter" || name == "s_ibl_lut" || name == "s_ambient_occlusion" ||
                name == "u_ambient_occlusion" || name == "u_cluster_grid" ||
                name == "u_cluster_camera" || name == "u_cluster_counts" ||
                name == "u_cluster_viewport" || name == "u_frame_inputs" ||
                name == "u_frame_time" || name == "u_frame_valid" ||
                name == "u_planar_projection" || name == "u_lighting_params" ||
                name == "u_probe_sh" || name == "s_lightmap" || name == "u_capture_mode" ||
                name == "u_rvt_bounds" || name == "u_rvt_layout" || name == "u_rvt_parameters" ||
                name == "u_rvt_height" || name == "s_rvt_atlas" || name == "s_rvt_table")
            {
                const auto type = name == "u_normal_transform" || name == "u_shadow_matrices" ||
                                          name == "u_planar_projection"
                                      ? bgfx::UniformType::Mat4
                                  : name == "s_virtual_shadow" || name == "s_shadow_map" ||
                                          name == "s_ibl_irradiance" || name == "s_ibl_prefilter" ||
                                          name == "s_ibl_lut" || name == "s_ambient_occlusion" ||
                                          name == "s_lightmap" || name == "s_rvt_atlas" ||
                                          name == "s_rvt_table"
                                      ? bgfx::UniformType::Sampler
                                      : bgfx::UniformType::Vec4;
                const auto expected_count =
                    name == "u_probe_sh"   ? 9
                    : name == "u_vsm_view" ? vsm_view_vectors
                    : name == "u_shadow_matrices" || name == "u_shadow_rects"
                        ? maximum_shadow_cascades
                        : 1;
                if (info.type != type || info.num != expected_count)
                {
                    SDL_LogError(SDL_LOG_CATEGORY_RENDER,
                                 "Invalid frame uniform %s in %s: type=%u count=%u expected=%u/%u",
                                 info.name, path.c_str(), static_cast<unsigned>(info.type),
                                 info.num, static_cast<unsigned>(type),
                                 static_cast<unsigned>(expected_count));
                    return false;
                }
                continue;
            }
            const bool sampler =
                std::any_of(shader.samplers.begin(), shader.samplers.end(),
                            [&name](const auto& item)
                            {
                                ENGINE_PROFILE_FUNCTION_PROBE();
                                return item->uniform_name == name;
                            });
            const bool parameter =
                std::any_of(shader.parameters.begin(), shader.parameters.end(),
                            [&name](const auto& item)
                            {
                                ENGINE_PROFILE_FUNCTION_PROBE();
                                return item->uniform_name == name;
                            });
            if (info.num != 1 || (sampler && parameter) ||
                (sampler ? info.type != bgfx::UniformType::Sampler
                         : !parameter || info.type != bgfx::UniformType::Vec4))
            {
                SDL_LogError(SDL_LOG_CATEGORY_RENDER,
                             "Undeclared or invalid shader uniform %s in %s", info.name,
                             path.c_str());
                return false;
            }
            reflected.push_back(info);
            if (opaque_stage)
            {
                opaque_inputs.emplace_back(name);
            }
        }
    }
    const auto declared = [&reflected](const std::string& name)
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        return std::any_of(reflected.begin(), reflected.end(),
                           [&name](const auto& info)
                           {
                               ENGINE_PROFILE_FUNCTION_PROBE();
                               return name == info.name;
                           });
    };
    const auto used_by_pbr_opaque = [&opaque_inputs](std::string_view name)
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        return std::ranges::find(opaque_inputs, name) != opaque_inputs.end();
    };
    for (const auto& parameter : shader.parameters)
    {
        if (std::any_of(uniforms.begin(), uniforms.end(), [&parameter](const auto& uniform)
                        {
            ENGINE_PROFILE_FUNCTION_PROBE();
            return uniform.name == parameter->uniform_name;
        }))
        {
            continue;
        }
        if (!declared(parameter->uniform_name))
        {
            SDL_LogError(SDL_LOG_CATEGORY_RENDER,
                         "Shader parameter %s has no reflected uniform in %s",
                         parameter->uniform_name.c_str(), path.c_str());
            return false;
        }
        GpuUniform uniform;
        uniform.name = parameter->uniform_name;
        uniform.used_by_pbr_opaque = used_by_pbr_opaque(uniform.name);
        uniform.handle = bgfx::createUniform(uniform.name.c_str(), bgfx::UniformType::Vec4);
        uniforms.push_back(std::move(uniform));
        if (!bgfx::isValid(uniforms.back().handle))
        {
            return false;
        }
    }
    for (const auto& item : shader.samplers)
    {
        if (!declared(item->uniform_name) ||
            item->stage >= bgfx::getCaps()->limits.maxTextureSamplers ||
            (item->source == schema::ShaderTextureSource::resource
                 ? item->stage > 4
                 : item->stage != (item->source == schema::ShaderTextureSource::scene_color   ? 9
                                   : item->source == schema::ShaderTextureSource::scene_depth ? 10
                                                                                              : 8)))
        {
            return false;
        }
        GpuSampler sampler;
        sampler.name = item->name;
        sampler.uniform_name = item->uniform_name;
        sampler.stage = item->stage;
        sampler.source = item->source;
        sampler.used_by_pbr_opaque = used_by_pbr_opaque(sampler.uniform_name);
        sampler.handle =
            bgfx::createUniform(item->uniform_name.c_str(), bgfx::UniformType::Sampler);
        samplers.push_back(std::move(sampler));
        if (!bgfx::isValid(samplers.back().handle))
        {
            return false;
        }
    }
    return true;
}

void GpuProgram::release() noexcept
{
    ENGINE_PROFILE_FUNCTION();
    resource.reset();
    if (bgfx::isValid(rvt_program))
    {
        bgfx::destroy(rvt_program);
    }
    if (bgfx::isValid(rvt_shader))
    {
        bgfx::destroy(rvt_shader);
    }
    rvt_program = BGFX_INVALID_HANDLE;
    rvt_shader = BGFX_INVALID_HANDLE;
    for (auto handle : {instanced_program, instanced_surface_program})
    {
        if (bgfx::isValid(handle))
        {
            bgfx::destroy(handle);
        }
    }
    if (bgfx::isValid(instanced_vertex_shader))
    {
        bgfx::destroy(instanced_vertex_shader);
    }
    instanced_vertex_shader = BGFX_INVALID_HANDLE;
    instanced_program = BGFX_INVALID_HANDLE;
    instanced_surface_program = BGFX_INVALID_HANDLE;

    if (bgfx::isValid(surface_program))
    {
        bgfx::destroy(surface_program);
        surface_program = BGFX_INVALID_HANDLE;
    }
    if (bgfx::isValid(surface_shader))
    {
        bgfx::destroy(surface_shader);
        surface_shader = BGFX_INVALID_HANDLE;
    }
    for (auto* handle : {&depth_program, &instanced_depth_program, &solid_depth_program,
                         &instanced_solid_depth_program, &pbr_opaque_program,
                         &instanced_pbr_opaque_program})
    {
        if (bgfx::isValid(*handle))
        {
            bgfx::destroy(*handle);
            *handle = BGFX_INVALID_HANDLE;
        }
    }
    if (bgfx::isValid(depth_shader))
    {
        bgfx::destroy(depth_shader);
        depth_shader = BGFX_INVALID_HANDLE;
    }
    if (bgfx::isValid(pbr_opaque_shader))
    {
        bgfx::destroy(pbr_opaque_shader);
        pbr_opaque_shader = BGFX_INVALID_HANDLE;
    }
    if (bgfx::isValid(program))
    {
        bgfx::destroy(program);
    }
    if (bgfx::isValid(vertex_shader))
    {
        bgfx::destroy(vertex_shader);
    }
    if (bgfx::isValid(fragment_shader))
    {
        bgfx::destroy(fragment_shader);
    }
    for (const auto& uniform : uniforms)
    {
        if (bgfx::isValid(uniform.handle))
        {
            bgfx::destroy(uniform.handle);
        }
    }
    program = BGFX_INVALID_HANDLE;
    vertex_shader = BGFX_INVALID_HANDLE;
    fragment_shader = BGFX_INVALID_HANDLE;
    uniforms.clear();
    for (const auto& sampler : samplers)
    {
        if (bgfx::isValid(sampler.handle))
        {
            bgfx::destroy(sampler.handle);
        }
    }
    samplers.clear();
}

bool GpuProgram::prepare_material(const Material& material, const TextureTable& textures,
                                  MaterialBinding& output) const
{
    ENGINE_PROFILE_FUNCTION();
    const auto* shader = &definition;
    output = {};
    MaterialBinding binding;
    binding.slot_identity = material.instance_id();
    binding.program = this;
    binding.cull = material.cull_mode();
    binding.blend = material.blend_mode();
    binding.values.resize(uniforms.size());
    for (const auto& parameter : shader->parameters)
    {
        std::array<double, 4> values{};
        std::size_t count = 0;
        const auto& value = parameter->default_value;
        switch (value.type)
        {
        case schema::MaterialValue::FloatValue:
        {
            float scalar = value.AsFloatValue()->value;
            if (parameter->exposed && !material.get_float(parameter->name, scalar))
            {
                return false;
            }
            values[0] = scalar;
            count = 1;
            break;
        }
        case schema::MaterialValue::Vec2Value:
        {
            Vec2 vector = value.AsVec2Value()->value;
            if (parameter->exposed && !material.get_v2(parameter->name, vector))
            {
                return false;
            }
            values = {vector.x(), vector.y(), 0.0, 0.0};
            count = 2;
            break;
        }
        case schema::MaterialValue::Vec3Value:
        {
            Vec3 vector = value.AsVec3Value()->value;
            if (parameter->exposed && !material.get_v3(parameter->name, vector))
            {
                return false;
            }
            values = {vector.x(), vector.y(), vector.z(), 0.0};
            count = 3;
            break;
        }
        case schema::MaterialValue::Vec4Value:
        {
            Vec4 vector = value.AsVec4Value()->value;
            if (parameter->exposed && !material.get_v4(parameter->name, vector))
            {
                return false;
            }
            values = {vector.x(), vector.y(), vector.z(), vector.w()};
            count = 4;
            break;
        }
        default:
            return false;
        }
        auto uniform = std::find_if(uniforms.begin(), uniforms.end(), [&parameter](const auto& item)
                                    {
            ENGINE_PROFILE_FUNCTION_PROBE();
            return item.name == parameter->uniform_name;
        });
        if (uniform == uniforms.end() || parameter->uniform_offset + count > 4)
        {
            return false;
        }
        for (std::size_t index = 0; index < count; ++index)
        {
            binding.values[uniform - uniforms.begin()][parameter->uniform_offset + index] =
                static_cast<float>(values[index]);
        }
    }
    for (const auto& sampler : samplers)
    {
        MaterialBinding::Sampler value;
        value.source = sampler.source;
        if (sampler.source != schema::ShaderTextureSource::resource)
        {
            binding.frame_textures = true;
        }
        else
        {
            const auto* texture = find_texture(textures, material.texture(sampler.name));
            if (!texture || !texture->resource.data<TextureResource>() ||
                texture->resource.data<TextureResource>()->cube_map)
            {
                return false;
            }
            value.texture = texture->handle.idx;
            value.flags = texture->sampler_flags;
        }
        binding.samplers.push_back(value);
    }
    binding.pbr_opaque = bgfx::isValid(pbr_opaque_program) &&
                         binding.blend == schema::BlendMode::opaque && !binding.frame_textures;
    for (std::size_t index = 0; index < uniforms.size(); ++index)
    {
        const auto& name = uniforms[index].name;
        if (name == "u_material")
        {
            const float mode = binding.values[index][2];
            binding.unlit = mode > .5F && mode < 1.5F;
            binding.pbr_opaque = binding.pbr_opaque && mode == 0.0F;
            binding.solid_depth =
                bgfx::isValid(solid_depth_program) && binding.values[index][3] == 0.0F;
        }
        else if (binding.pbr_opaque && name == "u_specular")
        {
            binding.pbr_opaque = binding.values[index][3] == 0.0F;
        }
        else if (binding.pbr_opaque && name == "u_hex_tiling")
        {
            binding.pbr_opaque = binding.values[index][0] == 0.0F;
        }
    }
    // 阴影约定只在阴影使用时报告不支持，普通颜色绘制不受影响。
    const auto parameter = [&](std::string_view name) -> const schema::ShaderParameterT*
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        const auto found = std::find_if(definition.parameters.begin(), definition.parameters.end(),
                                        [&](const auto& value)
                                        {
                                            ENGINE_PROFILE_FUNCTION_PROBE();
                                            return value->name == name;
                                        });
        return found == definition.parameters.end() ? nullptr : found->get();
    };
    const auto prepare_shadow = [&]()
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        if (const auto* cutoff = parameter("alphaCutoff"))
        {
            const auto* value = cutoff->default_value.AsFloatValue();
            if (!value)
            {
                return false;
            }
            binding.shadow_alpha[0] = value->value;
            if (cutoff->exposed && !material.get_float("alphaCutoff", binding.shadow_alpha[0]))
            {
                return false;
            }
        }
        if (!std::isfinite(binding.shadow_alpha[0]) || binding.shadow_alpha[0] < 0 ||
            binding.shadow_alpha[0] > 1)
        {
            return false;
        }
        if (binding.shadow_alpha[0] > 0)
        {
            const auto* color = parameter("baseColor");
            if (!color || !color->default_value.AsVec4Value())
            {
                return false;
            }
            auto tint = color->default_value.AsVec4Value()->value;
            const auto* texture = find_texture(textures, material.texture("baseColorTexture"));
            if (!texture || (color->exposed && !material.get_v4("baseColor", tint)))
            {
                return false;
            }
            binding.shadow_alpha[1] = static_cast<float>(tint.w());
            binding.shadow_texture = texture->handle;
            binding.shadow_texture_resource = texture->resource.handle();
            if (const auto* uv = parameter("uvTransform"))
            {
                if (!uv->default_value.AsVec4Value())
                {
                    return false;
                }
                auto transform = uv->default_value.AsVec4Value()->value;
                if (uv->exposed && !material.get_v4("uvTransform", transform))
                {
                    return false;
                }
                binding.shadow_uv = {
                    static_cast<float>(transform.x()), static_cast<float>(transform.y()),
                    static_cast<float>(transform.z()), static_cast<float>(transform.w())};
            }
        }
        return true;
    };
    binding.shadow_valid = prepare_shadow();
    const auto combine = [&](auto value)
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        binding.content_hash ^= std::hash<decltype(value)>{}(value) + 0x9e3779b9U +
                                (binding.content_hash << 6) + (binding.content_hash >> 2);
    };
    combine(reinterpret_cast<std::uintptr_t>(this));
    combine(static_cast<std::uint8_t>(binding.cull));
    combine(static_cast<std::uint8_t>(binding.blend));
    for (const auto& uniform : binding.values)
    {
        for (const auto value : uniform)
        {
            combine(value);
        }
    }
    for (const auto& sampler : binding.samplers)
    {
        combine(static_cast<std::uint8_t>(sampler.source));
        combine(sampler.texture);
        combine(sampler.flags);
    }
    output = std::move(binding);
    return true;
}

bool GpuProgram::bind_material(const MaterialBinding& material,
                               std::span<const bgfx::TextureHandle> frame_textures,
                               bool max_anisotropy, bool pbr_opaque) const
{
    ENGINE_PROFILE_FUNCTION();
    if (material.program != this || material.values.size() != uniforms.size() ||
        material.samplers.size() != samplers.size())
    {
        return false;
    }
    for (std::size_t index = 0; index < uniforms.size(); ++index)
    {
        const auto& uniform = uniforms[index];
        if (!pbr_opaque || uniform.used_by_pbr_opaque)
        {
            bgfx::setUniform(uniform.handle, material.values[index].data());
        }
    }
    for (std::size_t index = 0; index < samplers.size(); ++index)
    {
        const auto& sampler = samplers[index];
        if (pbr_opaque && !sampler.used_by_pbr_opaque)
        {
            continue;
        }
        const auto& value = material.samplers[index];
        bgfx::TextureHandle texture{value.texture};
        std::uint32_t flags = UINT32_MAX;
        if (value.source != schema::ShaderTextureSource::resource)
        {
            const auto source = static_cast<std::size_t>(value.source) - 1;
            if (source >= frame_textures.size())
            {
                return false;
            }
            texture = frame_textures[source];
        }
        else if (max_anisotropy)
        {
            flags = value.flags | BGFX_SAMPLER_MIN_ANISOTROPIC | BGFX_SAMPLER_MAG_ANISOTROPIC;
        }
        if (!bgfx::isValid(texture))
        {
            return false;
        }
        bgfx::setTexture(sampler.stage, sampler.handle, texture, flags);
    }
    return true;
}

} // namespace engine::render_detail
