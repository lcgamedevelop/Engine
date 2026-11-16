#include "editor/render/render_feature_commands.hpp"

#include "editor/commands/components/component_edit.hpp"
#include "editor/commands/scene_commands.hpp"
#include "runtime/render/passes/atmosphere_pass.hpp"
#include "runtime/render/render_feature_names.hpp"
#include "runtime/render/render_system.hpp"
#include "runtime/render/scene_render_settings.hpp"
#include "runtime/scene/environment_component.hpp"
#include "runtime/scene/mesh_renderer.hpp"
#include "runtime/schema/control_json_generated.hpp"

#include <array>
#include <limits>
#include <utility>

namespace engine::editor
{
namespace
{
using Json = nlohmann::json;

Json state_json(const RenderFeatureState& state)
{
    return {{"requested", state.requested},
            {"supported", state.supported},
            {"effective", state.effective},
            {"reason", render_detail::feature_reason_name(state.reason)},
            {"persistent", false}};
}

Json meshlet_sample_json(const MeshletGpuStats& value)
{
    constexpr std::array names{"disabled", "unsupported", "pending", "ready", "failed"};
    return {{"state", names[static_cast<std::size_t>(value.state)]},
            {"worldEpoch", std::to_string(value.world_epoch)},
            {"observer", {std::to_string(value.observer[0]), std::to_string(value.observer[1])}},
            {"frameNumber", value.frame_number},
            {"candidatePairs", value.candidate_pairs},
            {"candidateTriangles", value.candidate_triangles},
            {"visiblePairs", value.visible_pairs},
            {"nonemptyCommands", value.nonempty_commands},
            {"visibleTriangles", value.visible_triangles}};
}

Json features_json(const RenderSystem& renderer)
{
    Json result = Json::object();
    for (std::size_t index = 0; index < render_detail::feature_names.size(); ++index)
    {
        result[std::string(render_detail::feature_names[index])] =
            state_json(renderer.feature_state(static_cast<RenderFeature>(index)));
    }
    return result;
}

Json mesh_settings(const MeshRenderer& mesh)
{
    return {{"sceneId", std::to_string(mesh.node().scene()->id())},
            {"nodeId", std::to_string(mesh.node().id())},
            {"scope", "component"},
            {"staticInstancingEnabled", mesh.static_instancing_enabled()},
            {"staticLodEnabled", mesh.static_lod_enabled()},
            {"rvtRole", schema::EnumNameRvtRole(mesh.rvt_role())}};
}

class MeshRenderEdit final : public EditorEdit
{
public:
    MeshRenderEdit(WorldSystem& world, std::shared_ptr<std::uint64_t> identity,
                   const MeshRenderer& mesh, std::array<std::uint8_t, 3> after,
                   std::array<bool, 3> changed)
        : world_(world), identity_(std::move(identity)), scene_id_(mesh.node().scene()->id()),
          node_id_(mesh.node().id()), instance_(mesh.instance_id()),
          before_{std::uint8_t(mesh.static_instancing_enabled()),
                  std::uint8_t(mesh.static_lod_enabled()), std::uint8_t(mesh.rvt_role())},
          after_(after), changed_(changed)
    {
    }
    bool apply(std::string& error) override
    {
        return change(before_, after_, error);
    }
    bool revert(std::string& error) override
    {
        return change(after_, before_, error);
    }

private:
    bool change(const std::array<std::uint8_t, 3>& expected,
                const std::array<std::uint8_t, 3>& replacement, std::string& error)
    {
        auto* scene = world_.scene();
        const auto node =
            scene && scene->id() == scene_id_ && identity_ ? scene->find_node(*identity_) : Node{};
        auto mesh = node ? node.get_component<MeshRenderer>() : MeshRenderer{};
        if (!mesh || (node.id() == node_id_ && mesh.instance_id() != instance_) ||
            (changed_[0] && mesh.static_instancing_enabled() != expected[0]) ||
            (changed_[1] && mesh.static_lod_enabled() != bool(expected[1])) ||
            (changed_[2] && std::uint8_t(mesh.rvt_role()) != expected[2]))
        {
            error = "Mesh render properties or their component identity changed outside this edit";
            return false;
        }
        if (changed_[0] && !mesh.set_static_instancing_enabled(replacement[0]))
        {
            error = "Cannot update mesh instancing property";
            return false;
        }
        if (changed_[1] && !mesh.set_static_lod_enabled(replacement[1]))
        {
            const bool restored = !changed_[0] || mesh.set_static_instancing_enabled(expected[0]);
            error = restored ? "Cannot update mesh LOD property"
                             : "Cannot restore mesh render properties";
            return false;
        }
        if (changed_[2] && !mesh.set_rvt_role(schema::RvtRole(replacement[2])))
        {
            const bool restored_lod = !changed_[1] || mesh.set_static_lod_enabled(expected[1]);
            const bool restored_instancing =
                !changed_[0] || mesh.set_static_instancing_enabled(expected[0]);
            error = restored_lod && restored_instancing ? "Cannot set RVT role"
                                                        : "Cannot restore mesh properties";
            return false;
        }
        node_id_ = node.id();
        instance_ = mesh.instance_id();
        return true;
    }
    WorldSystem& world_;
    std::shared_ptr<std::uint64_t> identity_;
    std::uint64_t scene_id_;
    std::uint64_t node_id_;
    std::uint64_t instance_;
    std::array<std::uint8_t, 3> before_;
    std::array<std::uint8_t, 3> after_;
    std::array<bool, 3> changed_;
};
} // namespace

bool register_render_feature_commands(RenderSystem& renderer, WorldSystem& world,
                                      EditorCommands& commands, SceneCommands& scenes)
{
    return commands.register_command(
               "get_rvt_status", false,
               [&renderer](const Json& args, Json& result, std::unique_ptr<EditorEdit>&,
                           std::string&)
               {
                   RvtStatus status;
                   if (!args.empty() || !renderer.rvt_status(status))
                   {
                       return false;
                   }
                   result = {{"requested", status.requested},
                             {"supported", status.supported},
                             {"effective", status.effective},
                             {"reason", status.reason},
                             {"deviceGeneration", std::to_string(status.device_generation)},
                             {"demandRevision", std::to_string(status.demand_revision)},
                             {"cacheRevision", std::to_string(status.cache_revision)},
                             {"readyPages", status.ready_pages},
                             {"requiredPages", status.required_pages},
                             {"residentBytes", std::to_string(status.resident_bytes)}};
                   return true;
               }) &&
           commands.register_command(
               "get_rvt_debug_options", false,
               [&renderer](const Json& args, Json& result, std::unique_ptr<EditorEdit>&,
                           std::string&)
               {
                   RvtDebugOptions value;
                   std::uint64_t revision = 0;
                   if (!args.empty() || !renderer.rvt_debug_options(value, revision))
                   {
                       return false;
                   }
                   constexpr const char* modes[]{"overview", "virtual_texture", "physical_atlas",
                                                 "page_table"};
                   constexpr const char* channels[]{"base_color", "normal", "roughness", "metallic",
                                                    "ao"};
                   result = {{"mode", modes[unsigned(value.mode)]},
                             {"channel", channels[unsigned(value.channel)]},
                             {"mip", value.mip},
                             {"revision", std::to_string(revision)}};
                   return true;
               }) &&
           commands.register_command(
               "set_rvt_debug_options", false,
               [&renderer](const Json& args, Json& result, std::unique_ptr<EditorEdit>&,
                           std::string& error)
               {
                   RvtDebugOptions value;
                   std::uint64_t revision = 0;
                   if (!args.is_object() || args.empty() ||
                       !renderer.rvt_debug_options(value, revision))
                   {
                       return false;
                   }
                   constexpr std::array modes{"overview", "virtual_texture", "physical_atlas",
                                              "page_table"};
                   constexpr std::array channels{"base_color", "normal", "roughness", "metallic",
                                                 "ao"};
                   for (const auto& [key, entry] : args.items())
                   {
                       if (key == "mode" && entry.is_string())
                       {
                           auto i = std::find(modes.begin(), modes.end(), entry.get<std::string>());
                           if (i == modes.end())
                           {
                               return false;
                           }
                           value.mode = RvtDebugMode(i - modes.begin());
                       }
                       else if (key == "channel" && entry.is_string())
                       {
                           auto i = std::find(channels.begin(), channels.end(),
                                              entry.get<std::string>());
                           if (i == channels.end())
                           {
                               return false;
                           }
                           value.channel = RvtDebugChannel(i - channels.begin());
                       }
                       else if (key == "mip" && entry.is_number_integer() && entry >= -1 &&
                                entry <= 8)
                       {
                           value.mip = entry.get<std::int32_t>();
                       }
                       else
                       {
                           error = "Invalid RVT debug option";
                           return false;
                       }
                   }
                   if (!renderer.set_rvt_debug_options(value) ||
                       !renderer.rvt_debug_options(value, revision))
                   {
                       return false;
                   }
                   result = {{"revision", std::to_string(revision)}};
                   return true;
               }) &&
           commands.register_command(
               "get_mesh_render_settings", false,
               [&world](const Json& arguments, Json& result, std::unique_ptr<EditorEdit>&,
                        std::string& error)
               {
                   if (!arguments.is_object() || arguments.size() != 2)
                   {
                       return false;
                   }
                   const auto node = components::target(world, arguments, error);
                   const auto mesh = node ? node.get_component<MeshRenderer>() : MeshRenderer{};
                   if (!mesh)
                   {
                       error = "Selected node has no MeshRenderer";
                       return false;
                   }
                   result = mesh_settings(mesh);
                   return true;
               }) &&
           commands.register_command(
               "set_mesh_render_settings", true,
               [&world, &scenes](const Json& arguments, Json& result,
                                 std::unique_ptr<EditorEdit>& edit, std::string& error)
               {
                   if (!components::fields(
                           arguments, {"staticInstancingEnabled", "staticLodEnabled", "rvtRole"},
                           error) ||
                       arguments.size() <= 2)
                   {
                       return false;
                   }
                   const auto node = components::target(world, arguments, error);
                   const auto mesh = node ? node.get_component<MeshRenderer>() : MeshRenderer{};
                   if (!mesh)
                   {
                       error = "Selected node has no MeshRenderer";
                       return false;
                   }
                   std::array<std::uint8_t, 3> after{std::uint8_t(mesh.static_instancing_enabled()),
                                                     std::uint8_t(mesh.static_lod_enabled()),
                                                     std::uint8_t(mesh.rvt_role())};
                   std::array<bool, 3> changed{};
                   constexpr std::array keys{"staticInstancingEnabled", "staticLodEnabled"};
                   for (std::size_t index = 0; index < keys.size(); ++index)
                   {
                       if (!arguments.contains(keys[index]))
                       {
                           continue;
                       }
                       if (!arguments[keys[index]].is_boolean())
                       {
                           error = "Mesh render properties must be boolean";
                           return false;
                       }
                       after[index] = arguments[keys[index]].get<bool>();
                       changed[index] = true;
                   }
                   if (arguments.contains("rvtRole"))
                   {
                       schema::RvtRole role;
                       if (!schema::control_json::read(arguments["rvtRole"], role, error, false,
                                                       "rvtRole"))
                       {
                           return false;
                       }
                       after[2] = std::uint8_t(role);
                       changed[2] = true;
                   }
                   auto identity = scenes.node_identity(node.id());
                   if (!identity)
                   {
                       return false;
                   }
                   edit = std::make_unique<MeshRenderEdit>(world, std::move(identity), mesh, after,
                                                           changed);
                   result = mesh_settings(mesh);
                   result["staticInstancingEnabled"] = bool(after[0]);
                   result["staticLodEnabled"] = bool(after[1]);
                   result["rvtRole"] = schema::EnumNameRvtRole(schema::RvtRole(after[2]));
                   return true;
               }) &&
           commands.register_command(
               "get_render_features", false,
               [&renderer, &world](const Json& arguments, Json& result,
                                   std::unique_ptr<EditorEdit>&, std::string& error)
               {
                   if (!arguments.is_object())
                   {
                       return false;
                   }
                   for (const auto& item : arguments.items())
                   {
                       if (item.key() != "sceneId" && item.key() != "nodeId" &&
                           item.key() != "slot")
                       {
                           error = "Unknown render feature query field";
                           return false;
                       }
                   }
                   if (arguments.contains("sceneId") != arguments.contains("nodeId") ||
                       (arguments.contains("slot") && !arguments.contains("sceneId")))
                   {
                       error =
                           "Selected features require sceneId and nodeId; material requires slot";
                       return false;
                   }
                   result = {{"renderer",
                              {{"scope", "world_summary"},
                               {"features", features_json(renderer)},
                               {"frameNumber", renderer.frame_number()},
                               {"implementation",
                                {{"frame_view_uniforms",
                                  {{"active", renderer.gpu_ready()}, {"mutable", false}}}}}}}};
                   const auto settings = renderer.settings();
                   if (const auto* scene = world.scene())
                   {
                       const auto camera = scene->active_camera();
                       if (camera)
                       {
                           CameraSettings camera_settings;
                           if (!camera.get_settings(camera_settings))
                           {
                               error = "Cannot read camera settings";
                               return false;
                           }
                           result["camera"] = {
                               {"sceneId", std::to_string(scene->id())},
                               {"nodeId", std::to_string(camera.node().id())},
                               {"scope", "component"},
                               {"frustumCullingEnabled", camera_settings.frustum_culling_enabled}};
                       }
                       render_detail::SceneRenderComponents selected;
                       if (!render_detail::scene_render_components(*world.scene(), selected))
                       {
                           error = "Active scene has conflicting render components";
                           return false;
                       }
                       if (const auto environment = selected.environment)
                       {
                           EnvironmentSettings environment_settings;
                           if (!environment.get_settings(environment_settings))
                           {
                               error = "Cannot read environment settings";
                               return false;
                           }
                           const bool requested = environment_settings.atmosphere_enabled;
                           const bool supported =
                               renderer.gpu_ready() && render_detail::AtmospherePass::supported();
                           result["scene"] = {
                               {"sceneId", std::to_string(scene->id())},
                               {"nodeId", std::to_string(environment.node().id())},
                               {"features",
                                {{"atmosphere",
                                  {{"requested", requested},
                                   {"supported", supported},
                                   {"effective",
                                    requested && supported && settings.atmosphere_submitted},
                                   {"reason", !renderer.gpu_ready() ? "device_unavailable"
                                              : !supported          ? "device_unsupported"
                                              : requested && !settings.atmosphere_sun_valid
                                                  ? "sun_unavailable"
                                              : requested && !settings.atmosphere_submitted
                                                  ? "no_applicable_draws"
                                                  : "none"},
                                   {"persistent", true}}}}}};
                           auto meshlets =
                               state_json(renderer.feature_state(RenderFeature::meshlet_culling));
                           meshlets["persistent"] = true;
                           result["scene"]["features"]["meshlet_culling"] = std::move(meshlets);
                       }
                   }
                   if (arguments.contains("sceneId"))
                   {
                       const auto node = components::target(world, arguments, error);
                       if (!node)
                       {
                           return false;
                       }
                       if (const auto camera = node.get_component<Camera>())
                       {
                           CameraSettings camera_settings;
                           if (!camera.get_settings(camera_settings))
                           {
                               error = "Cannot read camera settings";
                               return false;
                           }
                           result["camera"] = {
                               {"sceneId", arguments["sceneId"]},
                               {"nodeId", arguments["nodeId"]},
                               {"scope", "component"},
                               {"frustumCullingEnabled", camera_settings.frustum_culling_enabled}};
                       }
                       if (const auto mesh = node.get_component<MeshRenderer>())
                       {
                           result["mesh"] = mesh_settings(mesh);
                       }
                       if (arguments.contains("slot"))
                       {
                           if (!arguments["slot"].is_number_unsigned() &&
                               (!arguments["slot"].is_number_integer() || arguments["slot"] < 0))
                           {
                               return false;
                           }
                           const auto slot = arguments["slot"].get<std::size_t>();
                           const auto mesh = node.get_component<MeshRenderer>();
                           const auto material = mesh ? mesh.material(slot) : Material{};
                           if (!material)
                           {
                               error = "Selected material slot is unavailable";
                               return false;
                           }
                           float enabled = 0;
                           const bool supported = material.get_float("hexTilingEnabled", enabled);
                           result["material"] = {
                               {"sceneId", arguments["sceneId"]},
                               {"nodeId", arguments["nodeId"]},
                               {"slot", slot},
                               {"features",
                                {{"hex_tiling",
                                  {{"requested", supported && enabled > 0.5F},
                                   {"supported", supported},
                                   {"ready", supported && enabled > 0.5F},
                                   {"scope", "material_parameter"},
                                   {"effective", nullptr},
                                   {"reason", supported ? "none" : "shader_unsupported"},
                                   {"persistent", true}}}}}};
                       }
                   }
                   return true;
               }) &&
           commands.register_command(
               "set_render_debug_feature", false,
               [&renderer](const Json& arguments, Json& result, std::unique_ptr<EditorEdit>&,
                           std::string& error)
               {
                   RenderFeature feature{};
                   if (!arguments.is_object() || arguments.size() != 2 ||
                       !arguments.contains("feature") || !arguments["feature"].is_string() ||
                       !arguments.contains("enabled") || !arguments["enabled"].is_boolean() ||
                       !render_detail::parse_feature(
                           arguments["feature"].get_ref<const std::string&>(), feature))
                   {
                       error = "Requires a temporary debug feature and boolean enabled";
                       return false;
                   }
                   if (!renderer.set_debug_feature(feature, arguments["enabled"].get<bool>()))
                   {
                       error = "Requested renderer feature is unsupported or device unavailable";
                       return false;
                   }
                   result = state_json(renderer.feature_state(feature));
                   result["feature"] = arguments["feature"];
                   result["frameNumber"] = renderer.frame_number();
                   return true;
               }) &&
           commands.register_command(
               "get_render_submission_stats", false,
               [&renderer](const Json& arguments, Json& result, std::unique_ptr<EditorEdit>&,
                           std::string& error)
               {
                   if (!arguments.is_object() || !arguments.empty())
                   {
                       error = "Submission statistics has no arguments";
                       return false;
                   }
                   Json rows = Json::array();
                   for (const auto& row : renderer.submission_stats())
                   {
                       rows.push_back(
                           {{"frameNumber", row.frame_number},
                            {"viewId", row.view_id},
                            {"candidateMeshes", row.candidate_meshes},
                            {"culledMeshes", row.culled_meshes},
                            {"visibleMeshes", row.visible_meshes},
                            {"drawCalls", row.draw_calls},
                            {"directInstanceBatches", row.direct_instance_batches},
                            {"directInstances", row.direct_instances},
                            {"directTriangles", row.direct_triangles},
                            {"indirectBatches", row.indirect_batches},
                            {"indirectCommands", row.indirect_commands},
                            {"meshletInstancedBatches", row.meshlet_instanced_batches},
                            {"meshletCandidatePairs", row.meshlet_candidate_pairs},
                            {"meshletCandidateTriangles", row.meshlet_candidate_triangles},
                            {"meshletMissingData", row.meshlet_missing_data},
                            {"meshletIncompatibleDraws", row.meshlet_incompatible_draws},
                            {"meshletBudgetFallbacks", row.meshlet_budget_fallbacks},
                            {"meshletGpu", meshlet_sample_json(row.meshlet_gpu)}});
                   }
                   result = {{"frameNumber", renderer.frame_number()},
                             {"views", rows},
                             {"source", "scene_mesh_submission"},
                             {"gpuCompletion", "not_measured"}};
                   return true;
               }) &&
           commands.register_command(
               "draw_debug_lines", false,
               [&renderer](const Json& arguments, Json& result, std::unique_ptr<EditorEdit>&,
                           std::string& error)
               {
                   if (!arguments.is_object() || !arguments.contains("lines") ||
                       !arguments["lines"].is_array() || arguments["lines"].empty() ||
                       arguments["lines"].size() > 4096)
                   {
                       error = "Debug draw requires between 1 and 4096 lines";
                       return false;
                   }
                   for (const auto& item : arguments.items())
                   {
                       if (item.key() != "lines" && item.key() != "lifetimeFrames")
                       {
                           return false;
                       }
                   }
                   std::uint16_t frames = 1;
                   if (arguments.contains("lifetimeFrames"))
                   {
                       if (!arguments["lifetimeFrames"].is_number_integer() ||
                           arguments["lifetimeFrames"] < 1 || arguments["lifetimeFrames"] > 600)
                       {
                           return false;
                       }
                       frames = arguments["lifetimeFrames"].get<std::uint16_t>();
                   }
                   std::vector<DebugLine> lines;
                   for (const auto& item : arguments["lines"])
                   {
                       if (!item.is_object() || !item.contains("start") || !item.contains("end"))
                       {
                           return false;
                       }
                       for (const auto& field : item.items())
                       {
                           if (field.key() != "start" && field.key() != "end" &&
                               field.key() != "color" && field.key() != "depthTest")
                           {
                               return false;
                           }
                       }
                       DebugLine line;
                       if (!components::vector(item["start"], line.start) ||
                           !components::vector(item["end"], line.end))
                       {
                           return false;
                       }
                       if (item.contains("color"))
                       {
                           if ((!item["color"].is_number_unsigned() &&
                                !item["color"].is_number_integer()) ||
                               item["color"] < 0 ||
                               item["color"] > std::numeric_limits<std::uint32_t>::max())
                           {
                               return false;
                           }
                           line.rgba = item["color"].get<std::uint32_t>();
                       }
                       if (item.contains("depthTest"))
                       {
                           if (!item["depthTest"].is_boolean())
                           {
                               return false;
                           }
                           line.depth_test = item["depthTest"].get<bool>();
                       }
                       lines.push_back(line);
                   }
                   if (!renderer.draw_debug_lines(lines, frames))
                   {
                       error = "Debug drawing is disabled, unavailable or exceeds its budget";
                       return false;
                   }
                   result = {{"acceptedLines", lines.size()},
                             {"lifetimeFrames", frames},
                             {"frameNumber", renderer.frame_number()},
                             {"status", "accepted"}};
                   return true;
               });
}
} // namespace engine::editor
