# Meshlet 标准测试场景

菜单中的 **Meshlets and GPU culling** 使用公开几何与标准 PBR 材质，
通过现有 Gameplay、资源管线及渲染器运行。场景总开关默认关闭；点击
`Scene Meshlets` 修改当前 `EnvironmentComponent`，返回菜单后再次进入恢复源场景设置。
架构见 [Meshlet 与 GPU 剔除](../../../docs/runtime/class-diagrams/meshlet-culling.md)。

## 构件与来源

`assets/scenes/meshlet-culling.scene` 保存镜头、光照及环境，
`assets/scripts/modules/meshlet-culling.ts` 组织固定的 29 个对象和 UI。

| 构件 | 验证用途 |
| --- | --- |
| 18 个共享球体实例、两个材质槽 | 材质分组、实例压缩及导入 LOD |
| 非均匀、镜像、关闭静态实例化的三个球体 | 保守剔除和单实例间接路径 |
| 四个拱门 | 单面曲面与共享几何 |
| 穿过视野边缘的双面长墙 | 跳过法线锥后仍执行 Meshlet 视锥剔除 |
| 普通方块地面 | 没有附加数据时保留普通绘制 |
| 使用未声明兼容 Shader 的球体 | 明确材质不适用的普通绘制 |
| 半透明球体 | 保持透明排序与普通通道 |

`meshlet-sphere.mesh`、`meshlet-arch.mesh`、`meshlet-wall.mesh` 由引擎建模 MCP
的 sphere、arch、plane 基元保存，走正常 Mesh 导入。`meshlet-lod-sphere.mesh`
复用本项目的 `render-lod-sphere.mesh` 公开几何，分成两个材质范围并生成各级 LOD／Meshlet。
普通自定义材质复用本项目 `pbr.shader`，不声明 Meshlet 兼容性。
这些资产不包含私有模型、纹理、脚本或购买清单。

各资产在邻接 meta 中使用 `[meshlets].enabled=true`；普通地面不生成专用数据。
资源选择与场景总开关独立。源 Mesh 仍保留普通拓扑，关闭场景总开关不改写磁盘资源。

## 运行和检查

使用根 `ninja-engine-test-release` Preset 构建 Release 编辑器与 MCP 工具，按[项目入口](index.md#验证)
启动 showcase，等待编辑器就绪。将本次构建的工具路径和运行目录传给现有测试：

```sh
python3 projects/showcase/tests/render_features_test.py \
  --case meshlets \
  --server build/ninja-engine-test-release/mcp_app \
  --runtime-dir projects/showcase/cache/meshlet-session
```

测试连接已启动的编辑器，不负责启动或关闭进程；初始玩法须停留在菜单。
它使用真实 UI 点击和编辑器 MCP，验证：

- 场景开关由 TS 读回，编辑器也修改同一环境组件；不接受缺少环境身份的运行开关请求。
- 主视图使用间接范围；缺数据、Shader 不适用与透明对象保持普通路径。
- 导入结果每级均有 Meshlet；多材质与静态实例化同时生效。
- GPU 统计等待 ready，并核对世界、观察者、采样帧及实际保留数量；不把 CPU 候选当成 GPU 结果。
- 固定透视与正交镜头分别做同条件普通／Meshlet 截图对照。
- 关闭场景功能后仍能通过 `meshlet_bounds` 查看已有分簇边界；调试视图不反向开启运行功能。
- `meshlet_colors` 把同一 Meshlet 的实际三角形填成稳定色块，与包围球线框分别控制；
  连续帧颜色一致，关闭后恢复原 PBR 像素，可与线框同时显示。
- 同一组球体和拱门分别用实例化和普通路径绘制色块，核对表面覆盖完整；
  只排除普通参考图中色块边界的一像素邻域，不允许漏色像素把自己标记成边界。
- 返回菜单、重新进入后恢复源场景默认值，停止旧间接提交。

PNG 比较排除顶部 UI，每通道容差为 2/255，不容许超出容差的缺面或颜色变化。
原生 `engine.render_submission` 用二进制精确几何做严格线性 HDR 对照，同时覆盖预算溢出整批回退、
尚未准备 GPU 数据时的普通帧、LOD、镜像、材质、探针及阴影；不放宽原生像素阈值。

离线生成与校验由 `editor.mesh_lod` 覆盖多材质、各级 LOD、重复三角形、绕序损坏、
部分层级数据及关闭生成；`editor.mesh_lod_pipeline` 通过实际命令验证 meta、稳定 GUID、
完整发布、无变更不重建、错误输入和关闭后移除附加数据。

## 归档与边界

结果位于 `cache/visual-tests/meshlet-culling/<run-id>/`，包括：

- `ordinary.png`、`meshlets.png`；
- `orthographic-ordinary.png`、`orthographic-meshlets.png`；
- `bounds-while-disabled.png`；
- `cluster-colors.png`、`cluster-colors-repeat.png`、`cluster-colors-direct.png`、
  `cluster-colors-and-bounds.png`、
  `cluster-colors-restored.png`；
- `receipts.jsonl` 的会话、资源修订、运行／场景身份、提交帧和完整操作回执；
- `result.json` 的导入层级、CPU／GPU 计数、像素对照及通过状态。

截图和缓存不进入 Git。失败保留回执与失败帧，不能将仅完成命令当成画面通过。
实际验证结果和平台限制统一见[测试文档](../../../docs/testing.md#meshlet-与-gpu-剔除)。

私有项目沿同一导入、渲染和截图链路对照真实资源；它的全部内容及结果仅保存在自身忽略目录，
不向公开项目复制资产或清单。首期没有 Hi-Z 或 Mesh Shader；功能检查不代表性能收益。
