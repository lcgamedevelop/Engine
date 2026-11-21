#include "runtime/render/vsm/vsm_debug_view.hpp"
#include "runtime/performance/function_profile.hpp"
#include "runtime/performance/profiling.hpp"
#include "fs_vsm_debug.bin.h"
#include "runtime/font/font_system.hpp"
#include "runtime/render/passes/clustered_lighting_pass.hpp"
#include "runtime/render/passes/frame_output.hpp"
#include "runtime/render/render_constants.hpp"
#include "runtime/render/vsm/virtual_shadow_pass.hpp"
#include "runtime/resource/types/font_resource.hpp"
#include "vs_rvt_debug.bin.h"
#include <algorithm>
#include <array>
#include <bx/math.h>
#include <string>

namespace engine::render_detail
{
VsmDebugView::~VsmDebugView() noexcept
{
    ENGINE_PROFILE_FUNCTION();
    release_gpu();
}
bool VsmDebugView::prepare_font(ResourceLoader& resources)
{
    ENGINE_PROFILE_FUNCTION();
    const auto prepare = [&](std::string_view path, ResourceReference& resource,
                             std::unique_ptr<ResourceLoad>& pending)
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        if (resource)
        {
            return true;
        }
        if (!pending)
        {
            if (resource.acquire(&resources, path))
            {
                return true;
            }
            if (!resources.begin_load(path, pending))
            {
                return false;
            }
        }
        ResourceLoadProgress progress;
        if (pending->status(progress) != ResourceLoadStatus::ready)
        {
            return false;
        }
        ResourceHandle handle;
        if (!pending->take(handle) || !resource.adopt(&resources, handle))
        {
            (void)resources.release(handle);
            return false;
        }
        pending.reset();
        return true;
    };
    const bool font = prepare("builtin://fonts/diagnostic", font_, font_pending_);
    const bool shader = prepare("builtin://shaders/font", shader_, shader_pending_);
    return font && shader;
}
bool VsmDebugView::draw(const VsmStatus& status, const VirtualShadowPass* pass,
                        const ClusteredLightingPass* lighting, const VsmDebugOptions& options,
                        const FrameOutput& output)
{
    ENGINE_PROFILE_FUNCTION();
    const FontResource* font = nullptr;
    if (!font_ || !shader_ || font_.loader()->get(font_.handle(), font) != ResourceError::none)
    {
        return false;
    }
    SceneWorkProgress progress;
    SceneWorkBudget budget;
    if (fonts_.prepare_font(font_.handle(), shader_.handle(), budget, progress) !=
        SceneWorkState::ready)
    {
        return false;
    }
    if (!bgfx::isValid(program_))
    {
        const auto vs = bgfx::createShader(bgfx::copy(vs_rvt_debug, sizeof(vs_rvt_debug)));
        const auto fs = bgfx::createShader(bgfx::copy(fs_vsm_debug, sizeof(fs_vsm_debug)));
        if (!bgfx::isValid(vs) || !bgfx::isValid(fs))
        {
            if (bgfx::isValid(vs))
            {
                bgfx::destroy(vs);
            }
            if (bgfx::isValid(fs))
            {
                bgfx::destroy(fs);
            }
            return false;
        }
        program_ = bgfx::createProgram(vs, fs, true);
        options_ = bgfx::createUniform("u_vsm_debug", bgfx::UniformType::Vec4);
    }
    if (!bgfx::isValid(program_) || !bgfx::isValid(options_))
    {
        return false;
    }
    const float width = output.width(), height = output.height();
    const float scale = std::clamp(width / 1280.F, 1.F, 2.F);
    const bool overview =
        options.mode == VsmDebugMode::overview || options.mode >= VsmDebugMode::residency;
    const unsigned count = overview ? 3 : 1;
    const bool horizontal = width >= 700;
    const float tile = std::max(
        24.F,
        std::min({180.F * scale,
                  (width - 32 * scale) / (horizontal ? float(count) : 1.F) - 8 * scale,
                  (height * .45F - 120 * scale) / (horizontal ? 1.F : float(count)) - 24 * scale}));
    const float panel_width =
        std::min(width - 16 * scale,
                 std::max(304.F * scale, horizontal ? count * (tile + 8 * scale) + 16 * scale
                                                    : tile + 24 * scale));
    const float panel_height =
        (horizontal ? tile + 24 * scale : count * (tile + 24 * scale)) + 112 * scale;
    const float x = 8 * scale, y = height - panel_height - 8 * scale;
    bgfx::setViewName(vsm_debug_view, "VSM live textures");
    bgfx::setViewFrameBuffer(vsm_debug_view, output.target());
    bgfx::setViewRect(vsm_debug_view, 0, 0, output.width(), output.height());
    bgfx::setViewClear(vsm_debug_view, BGFX_CLEAR_NONE);
    bgfx::setViewMode(vsm_debug_view, bgfx::ViewMode::Sequential);
    float projection[16];
    bx::mtxOrtho(projection, 0, width, height, 0, 0, 1000, 0, bgfx::getCaps()->homogeneousDepth);
    bgfx::setViewTransform(vsm_debug_view, nullptr, projection);
    if (lighting)
    {
        lighting->bind_view(vsm_debug_view, true);
    }
    struct Vertex
    {
        float x, y, z, u, v;
    };
    bgfx::VertexLayout layout;
    layout.begin()
        .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
        .end();
    const auto rectangle = [&](float left, float top, float size_x, float size_y, float mode,
                               bool available) -> bool
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        VsmGpuBinding binding;
        available = available && pass && lighting && pass->binding(binding);
        if (mode >= 0 && !available)
        {
            return true;
        }
        if (bgfx::getAvailTransientVertexBuffer(6, layout) < 6)
        {
            return false;
        }
        bgfx::TransientVertexBuffer vertices;
        bgfx::allocTransientVertexBuffer(&vertices, 6, layout);
        const auto point = [&](float px, float py, float u, float v)
        {
            ENGINE_PROFILE_FUNCTION_PROBE();
            return Vertex{px / width * 2 - 1, 1 - py / height * 2, 0, u, v};
        };
        const std::array data{point(left, top, 0, 0),
                              point(left, top + size_y, 0, 1),
                              point(left + size_x, top + size_y, 1, 1),
                              point(left, top, 0, 0),
                              point(left + size_x, top + size_y, 1, 1),
                              point(left + size_x, top, 1, 0)};
        std::copy(data.begin(), data.end(), reinterpret_cast<Vertex*>(vertices.data));
        const float parameters[]{mode, float(0), float(options.clipmap_level),
                                 available ? 1.F : 0.F};
        bgfx::setUniform(options_, parameters);
        if (available)
        {
            lighting->bind(true);
            bgfx::setTexture(10, binding.sampler, binding.atlas);
        }
        bgfx::setVertexBuffer(0, &vertices);
        bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_BLEND_ALPHA);
        bgfx::submit(vsm_debug_view, program_);
        return true;
    };
    const auto text = [&](std::string_view value, float tx, float ty) -> bool
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        FontGeometry geometry;
        FontLayout settings;
        settings.size = 12 * scale;
        if (!FontSystem::layout(*font, value, settings, geometry))
        {
            return false;
        }
        FontDraw draw;
        draw.view_id = vsm_debug_view;
        draw.shader = shader_.handle();
        draw.transform[12] = tx;
        draw.transform[13] = ty;
        return fonts_.draw(font_.handle(), geometry, draw);
    };
    if (!rectangle(x, y, panel_width, panel_height, -1, false))
    {
        return false;
    }
    if (!text("VSM / Live GPU depth", x + 8 * scale, y + 17 * scale))
    {
        return false;
    }
    const std::string summary = std::to_string(status.ready_pages) + " / " +
                                std::to_string(status.demanded_pages) + " pages | " +
                                std::to_string(status.resident_bytes / 1024) + " KiB";
    const std::string revisions = "Demand " + std::to_string(status.demand_revision) + " / Cache " +
                                  std::to_string(status.cache_revision);
    if (!text(summary, x + 8 * scale, y + 34 * scale) ||
        !text(revisions, x + 8 * scale, y + 51 * scale))
    {
        return false;
    }
    const std::string
        state =
            status.error.empty()
                ? std::array{"Disabled",       "Warming",     "Ready",
                             "Budget limited", "Unsupported", "Failed"}[std::size_t(status.state)]
                : status.error;
    if (!text(state, x + 8 * scale, y + 68 * scale))
    {
        return false;
    }
    constexpr const char* titles[]{"Virtual Depth", "Physical Atlas", "Page Table"};
    for (unsigned index = 0; index < count; ++index)
    {
        const auto mode = overview ? index : unsigned(options.mode) - 1;
        const float px = x + 8 * scale + (horizontal ? index * (tile + 8 * scale) : 0),
                    py = y + 80 * scale + (horizontal ? 0 : index * (tile + 24 * scale));
        if (!text(titles[mode], px, py + 12 * scale) ||
            !rectangle(px, py + 18 * scale, tile, tile, float(mode),
                       pass && lighting && status.state != VsmState::disabled))
        {
            return false;
        }
    }
    return text("Green: cached | Gold: new | Red: dirty | Purple: missing", x + 8 * scale,
                y + panel_height - 7 * scale);
}
void VsmDebugView::release_gpu() noexcept
{
    ENGINE_PROFILE_FUNCTION();
    if (bgfx::isValid(program_))
    {
        bgfx::destroy(program_);
    }
    if (bgfx::isValid(options_))
    {
        bgfx::destroy(options_);
    }
    program_ = BGFX_INVALID_HANDLE;
    options_ = BGFX_INVALID_HANDLE;
    font_.reset();
    shader_.reset();
    font_pending_.reset();
    shader_pending_.reset();
}
} // namespace engine::render_detail
