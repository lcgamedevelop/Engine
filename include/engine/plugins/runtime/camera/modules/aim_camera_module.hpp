#pragma once

#include "runtime/performance/function_profile.hpp"
#include "plugins/runtime/camera/camera_types.hpp"
namespace engine::plugins::camera
{
class AimCameraModule
{
public:
    using Data = schema::CameraAimDataT;
    struct Runtime
    {
        CameraPose pose;
    };
    static constexpr std::string_view type()
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        return "aim";
    }
    static constexpr CameraModuleStage stage()
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        return CameraModuleStage::aim;
    }
    static const Data* find(const schema::CameraControllerDataT& d, std::uint64_t id)
    {
        ENGINE_PROFILE_FUNCTION_PROBE();
        if (d.module_data)
        {
            for (const auto& v : d.module_data->aim)
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
