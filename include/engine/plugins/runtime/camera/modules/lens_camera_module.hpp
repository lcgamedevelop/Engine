#pragma once

#include "runtime/performance/function_profile.hpp"
#include "plugins/runtime/camera/camera_types.hpp"
namespace engine::plugins::camera
{
class LensCameraModule
{
public:
    using Data = schema::CameraLensDataT;
    struct Runtime
    {
        CameraPose pose;
    };
    static constexpr std::string_view type()
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        return "lens";
    }
    static constexpr CameraModuleStage stage()
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        return CameraModuleStage::lens;
    }
    static const Data* find(const schema::CameraControllerDataT& d, std::uint64_t id)
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        if (d.module_data)
        {
            for (const auto& v : d.module_data->lens)
            {
                if (v.id == id)
                {
                    return &v;
                }
            }
        }
        return nullptr;
    }
    [[nodiscard]] static bool validate(const Data&, const schema::CameraControllerDataT&,
                                       std::string&);
    [[nodiscard]] static bool seed(const Data&, const CameraPose& pose, Runtime& output)
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        output = {};
        output.pose = pose;
        return true;
    }
    [[nodiscard]] static bool evaluate(const Data&, const Runtime&, Runtime&,
                                       const CameraEvaluationContext&, CameraPose&);
};
} // namespace engine::plugins::camera
