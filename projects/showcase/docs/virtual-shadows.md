# 方向光虚拟阴影标准场景

菜单入口 **Virtual shadow maps**。公开场景由普通 PBR 方块搭成柱廊、屋顶梁、细杆、台阶和可移动箱体，
用于观察细投影、接触位置、缓存复用与动态失效。资源不包含私有神庙内容，也不要求生成 Meshlet 数据。

`assets/scenes/virtual-shadows.scene` 的 Sun 使用 `shadow_method = "virtual_shadow_map"`；
`cast_shadows` 是投影总开关，改回 `shadow_map` 停止反馈并回收 VSM 数据。
脚本通过普通 Light 组件的 `SetShadowMethod(0 | 1)` 修改同一状态。
CSM 始终作为首版缺页和非 VSM 接收通道的底图，不能用当前功能宣称减少全部阴影成本。

## 编辑器实时查看

- `get_vsm_status`：需求、有效页、缓存修订以及实际帧证据。
- `set_render_feature {"feature":"vsm_viewer","enabled":true}`：显示实时图像，false 关闭。
- `set_vsm_debug_options {"mode":"overview","clipmapLevel":-1}`：虚拟深度、物理图集、页表并排；
  还可选择 virtual_depth、physical_atlas、page_table、residency 或 invalidated。
- 调试开关不修改 Light；关闭 VSM 后仍可看到 Disabled 状态，不保留旧图集。

## 复现与归档

先启动 Release 测试编辑器并完成源资源导入，保持菜单页面：

```sh
python3 projects/showcase/tests/render_features_test.py \
  --server <release-mcp-app> --runtime-dir <editor-runtime-directory> --case vsm
```

复用现有编辑器 MCP 会话和截图流程，依次检查固定镜头预热、静止无重绘、移动箱体引起失效、
六种调试视图、非法级别拒绝、关闭回收、重新开启新代次和返回菜单清理。
图像与 `result.json`、`receipts.jsonl` 保存在记录根目录的 `records/validation/showcase/visual-tests/virtual-shadows/<run-id>/`。 [记录目录配置](../../../docs/runtime/performance.md)
每张图保留实际提交帧、VSM generation/view/demand/cache/debug 修订，不将命令受理当成 GPU 完成。

原生回归还检查页面预算、取消未发布页面、旧反馈拒绝、动态物体旧/新范围失效、
严格 HDR 静态对照和关闭后恢复 CSM。平台结果与剩余验证范围统一见
[引擎测试文档](../../../docs/testing.md#方向光虚拟阴影)。
