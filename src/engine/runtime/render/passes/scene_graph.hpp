#pragma once

#include "runtime/render/frame_graph_plan.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <initializer_list>

namespace engine::render_detail
{
// These IDs identify CPU encoding groups; each group retains its owned physical bgfx Views.
enum class ScenePass : FramePassId
{
    frame_geometry,
    rvt,
    planar_first,
    planar_second,
    vsm,
    lighting,
    vsm_publish,
    collect_visible,
    meshlets,
    depth,
    surface,
    depth_identity,
    feedback,
    ao,
    msaa_depth,
    sky,
    opaque,
    reflections,
    freeze,
    transparent,
    text,
    extensions,
    debug,
    tone_map,
    linear_capture,
    count
};

struct SceneResource
{
    enum : FrameResourceId
    {
        geometry,
        terrain,
        initial_shadow_cache,
        first_mirror_shadow_cache,
        second_mirror_shadow_cache,
        mirror0,
        mirror1,
        vsm_batch,
        lights,
        light_bindings,
        visible,
        mesh_streams,
        surface_depth,
        material_surface,
        depth_key,
        ao,
        msaa_depth,
        sky_color,
        sky_depth,
        opaque_color,
        opaque_specular,
        opaque_depth,
        reflected_color,
        initial_color,
        transparent_color,
        text_color,
        extension_color,
        extension_depth,
        linear_color,
        display_color
    };
};

struct SceneGraphKey
{
    bool combined_depth, reuse_surface, ssr, ao, vsm, msaa_depth, text, extensions, capture;
    std::array<bool, 2> mirrors{};
    bool operator==(const SceneGraphKey&) const = default;
};

struct ScenePreparationKey
{
    std::array<bool, 2> mirrors;
    bool operator==(const ScenePreparationKey&) const = default;
};

// Keeps the spans supplied to the generic compiler valid through compile(). No declaration
// storage survives compilation. Empty reads are valid for the first producer in a scope.
class ScenePassDeclarations final
{
public:
    void add(ScenePass pass, std::initializer_list<FrameResourceId> reads,
             std::initializer_list<FrameResourceId> writes, bool side_effect = false,
             std::span<const FrameResourceId> material_inputs = {})
    {
        assert(count_ < capacity);
        auto& storage = uses_[count_];
        const auto input_count = reads.size() + material_inputs.size();
        assert(input_count + writes.size() <= storage.size());
        std::copy(reads.begin(), reads.end(), storage.begin());
        std::copy(material_inputs.begin(), material_inputs.end(), storage.begin() + reads.size());
        std::copy(writes.begin(), writes.end(), storage.begin() + input_count);
        passes_[count_++] = {static_cast<FramePassId>(pass),
                             {storage.data(), input_count},
                             {storage.data() + input_count, writes.size()},
                             {},
                             side_effect};
    }
    [[nodiscard]] bool compile(FrameGraphPlan& plan, std::span<const FrameResourceId> imports,
                               std::span<const FrameResourceId> roots, FramePlanError& error) const
    {
        return plan.compile(std::span(passes_).first(count_), imports, roots, error);
    }

private:
    static constexpr auto capacity = static_cast<std::size_t>(ScenePass::count);
    std::array<std::array<FrameResourceId, 12>, capacity> uses_{};
    std::array<FramePassDeclaration, capacity> passes_{};
    std::size_t count_ = 0;
};

// The preparation scope exports these versions to the primary camera. The RVT entry describes
// the binding for this frame, including its fallback when the cache is not ready.
struct ScenePreparedInputs
{
    explicit ScenePreparedInputs(const std::array<bool, 2>& mirrors)
    {
        if (mirrors[0])
        {
            resources_[count_++] = SceneResource::mirror0;
        }
        if (mirrors[1])
        {
            resources_[count_++] = SceneResource::mirror1;
        }
    }

    [[nodiscard]] std::span<const FrameResourceId> all() const
    {
        return std::span(resources_).first(count_);
    }
    [[nodiscard]] std::span<const FrameResourceId> material() const
    {
        return all().subspan(2);
    }

private:
    std::array<FrameResourceId, 5> resources_{
        SceneResource::mesh_streams, SceneResource::light_bindings, SceneResource::terrain};
    std::size_t count_ = 3;
};

[[nodiscard]] inline bool compile_scene_preparation(FrameGraphPlan& plan,
                                                    const ScenePreparationKey& key,
                                                    FramePlanError& error)
{
    using P = ScenePass;
    using R = SceneResource;
    ScenePassDeclarations graph;
    graph.add(P::frame_geometry, {}, {R::geometry});
    graph.add(P::rvt, {R::geometry}, {R::terrain});
    // Successive observers may import a local-light tile validated by the preceding observer.
    // The initial cache may be empty; validity of individual tile contents is checked by
    // ShadowPass.
    FrameResourceId shadow_cache = R::initial_shadow_cache;
    if (key.mirrors[0])
    {
        graph.add(P::planar_first, {R::geometry, R::terrain, shadow_cache},
                  {R::mirror0, R::first_mirror_shadow_cache});
        shadow_cache = R::first_mirror_shadow_cache;
    }
    if (key.mirrors[1])
    {
        graph.add(P::planar_second, {R::geometry, R::terrain, shadow_cache},
                  {R::mirror1, R::second_mirror_shadow_cache});
        shadow_cache = R::second_mirror_shadow_cache;
    }
    graph.add(P::vsm, {R::geometry, R::terrain}, {R::vsm_batch});
    graph.add(P::lighting, {R::geometry, R::vsm_batch, shadow_cache}, {R::lights});
    graph.add(P::vsm_publish, {R::lights, R::vsm_batch}, {R::light_bindings});
    graph.add(P::collect_visible, {R::geometry, R::light_bindings}, {R::visible});
    graph.add(P::meshlets, {R::visible}, {R::mesh_streams});
    const ScenePreparedInputs exported(key.mirrors);
    constexpr FrameResourceId imports[]{R::initial_shadow_cache};
    return graph.compile(plan, imports, exported.all(), error);
}

[[nodiscard]] inline bool compile_main_scene(FrameGraphPlan& plan, const SceneGraphKey& key,
                                             FramePlanError& error)
{
    using P = ScenePass;
    using R = SceneResource;
    const ScenePreparedInputs imported(key.mirrors);
    ScenePassDeclarations graph;
    std::array<FrameResourceId, 7> inputs{};
    auto next_input = std::copy(imported.all().begin(), imported.all().end(), inputs.begin());
    if (key.reuse_surface)
    {
        *next_input++ = R::surface_depth;
        *next_input++ = R::material_surface;
    }
    else if (key.combined_depth)
    {
        graph.add(P::surface, {R::mesh_streams}, {R::surface_depth, R::material_surface});
    }
    else
    {
        graph.add(P::depth, {R::mesh_streams}, {R::surface_depth});
        if (key.ssr)
        {
            graph.add(P::surface, {R::surface_depth, R::mesh_streams}, {R::material_surface});
        }
    }
    if (key.ao || key.ssr || key.vsm)
    {
        graph.add(P::depth_identity, {R::surface_depth}, {R::depth_key});
    }
    if (key.vsm)
    {
        graph.add(P::feedback, {R::depth_key, R::surface_depth}, {}, true);
    }
    if (key.ao)
    {
        graph.add(P::ao, {R::surface_depth, R::depth_key}, {R::ao});
    }
    if (key.msaa_depth)
    {
        graph.add(P::msaa_depth, {R::mesh_streams}, {R::msaa_depth});
    }
    if (key.msaa_depth)
    {
        graph.add(P::sky, {R::light_bindings, R::msaa_depth}, {R::sky_color, R::sky_depth});
    }
    else
    {
        graph.add(P::sky, {R::light_bindings}, {R::sky_color, R::sky_depth});
    }
    if (key.ao)
    {
        graph.add(
            P::opaque, {R::sky_color, R::sky_depth, R::light_bindings, R::ao, R::mesh_streams},
            {R::opaque_color, R::opaque_specular, R::opaque_depth}, false, imported.material());
    }
    else
    {
        graph.add(P::opaque, {R::sky_color, R::sky_depth, R::light_bindings, R::mesh_streams},
                  {R::opaque_color, R::opaque_specular, R::opaque_depth}, false,
                  imported.material());
    }
    if (key.ssr)
    {
        graph.add(P::reflections,
                  {R::opaque_color, R::opaque_specular, R::surface_depth, R::material_surface,
                   R::depth_key},
                  {R::reflected_color});
    }
    const auto frozen = key.ssr ? R::reflected_color : R::opaque_color;
    graph.add(P::freeze, {frozen, R::opaque_depth}, {R::initial_color});
    graph.add(P::transparent,
              {R::initial_color, frozen, R::surface_depth, R::opaque_depth, R::mesh_streams},
              {R::transparent_color}, false, imported.material());
    FrameResourceId color = R::transparent_color;
    if (key.text)
    {
        graph.add(P::text, {color, R::opaque_depth}, {R::text_color});
        color = R::text_color;
    }
    FrameResourceId depth = R::opaque_depth;
    if (key.extensions)
    {
        // Registered draws may write the depth attachment. Treat the loaded result as a new
        // version even for an extension which only samples depth.
        graph.add(P::extensions, {color, depth}, {R::extension_color, R::extension_depth}, true);
        color = R::extension_color;
        depth = R::extension_depth;
    }
    graph.add(P::debug, {color, depth}, {R::linear_color});
    graph.add(P::tone_map, {R::linear_color}, {R::display_color});
    if (key.capture)
    {
        graph.add(P::linear_capture, {R::linear_color}, {}, true);
    }
    constexpr std::array<FrameResourceId, 1> roots{R::display_color};
    return graph.compile(plan, {inputs.begin(), next_input}, roots, error);
}
} // namespace engine::render_detail
