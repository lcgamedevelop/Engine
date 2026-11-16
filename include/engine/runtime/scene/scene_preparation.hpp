#pragma once

#include "runtime/scene/scene_loading.hpp"

namespace engine
{
// 借用由宿主登记；准备仅推进指定候选，状态查询不得执行工作。
class ScenePreparation
{
public:
    virtual ~ScenePreparation() noexcept = default;
    [[nodiscard]] virtual SceneWorkState prepare_scope(SceneScopeId scope,
                                                       const SceneWorkBudget& budget) = 0;
    [[nodiscard]] virtual bool scope_status(SceneScopeId scope,
                                            ScenePreparationSnapshot& output) const = 0;
    virtual void release_scope(SceneScopeId scope) noexcept = 0;
};
} // namespace engine
