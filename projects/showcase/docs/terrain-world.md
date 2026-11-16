# 大世界地形与 RVT

公开范例提供两个互补入口，全部使用可公开的标准资源：

| 菜单 | 源与场景 | 用途 |
| --- | --- | --- |
| Terrain | `terrain/laboratory.terrain`、`scenes/terrain.scene` | 1 米采样，观察洞穴、道路与分层 PBR，并进行局部挖填 |
| Terrain World | `terrain/world.terrain`、`scenes/terrain-world.scene` | 8×8 公里、16 米采样，验证四角流送、LOD、存档和 RVT |

大世界垂直范围为 -128～128 米，块边长为 16 个采样，最多 6 个 LOD，固定种子 42。
16 米采样有意用于控制标准验收的资源规模，不代表精细地形或第一人称洞穴的画质；
细节编辑使用实验场。地形表面用项目已有土壤与岩石贴图，编译为标准 PBR 层数组，
道路和画层改变权重，洞壁仍从相同的体积数据生成。

## 运行与查看

以 `projects/showcase` 启动编辑器，待资源导入完成后运行项目，在菜单选择 Terrain World。
界面上的 Southwest、Southeast、Northeast、Northwest 切换四角。
脚本先提交新视域，等对应显示与碰撞版本就绪后移动相机，再释放旧视域；不以固定延时猜加载完成。
Model、Controller、UI 分别位于 `scripts/models/terrain_world_model.ts`、
`scripts/controllers/terrain_world_controller.ts` 和 `ui/terrain-world.rml`。

生产开关保存在 World environment 节点的 `EnvironmentComponent.rvt.enabled`。
默认引擎关闭 RVT；此范例显式打开。关闭后保留源材质和地形，继续直接 PBR，释放页面缓存。

调试面板由编辑器 MCP 的 `set_render_feature` 开关：

```json
{"feature":"rvt_viewer","enabled":true}
```

`set_rvt_debug_options` 的 `mode` 可选 `overview`、`virtual_texture`、`physical_atlas`、`page_table`；
`channel` 可选 `base_color`、`normal`、`roughness`、`metallic`、`ao`，`mip=-1` 自动选择。
例如：

```json
{"mode":"physical_atlas","channel":"normal","mip":-1}
```

面板逐帧直接显示 GPU 贴图、页表与状态。查看器是临时编辑状态，不写进场景；关闭查看器不关闭生产 RVT。
`get_rvt_status` 查询 requested/effective、所需与就绪页面、显存和版本。
截图回执中的 `rvtDebugVisible`、`rvtDebugRevision`、`rvtDemandRevision`、`rvtCacheRevision`
和 `rvtReady` 用于确认截到的是目标画面。

## 无人操作测试

[terrain_world_test.py](../tests/terrain_world_test.py) 复用项目已有 MCP 会话与真实 UI 输入工具，
需要已经启动、完成导入并处于停止状态的编辑器。测试不创建第二套运行器，也不关闭调用方启动的编辑器。

```sh
python3 projects/showcase/tests/terrain_world_test.py \
  --server build/ninja-engine-release/mcp_app \
  --runtime-dir <editor-runtime-directory>
```

`--server` 需对应当前编辑器的构建目录；本地自定义预设可替换为其实际路径。
测试通过菜单进入场景，等待四个视域与 RVT 页面准备，逐一截图；然后挖地、保存、重置、恢复，
切换法线图集、关闭 RVT、关闭查看器，最后返回菜单。
失败保留原始回执和已完成截图，不把黑色清屏或仅创建文件判为成功。

每次运行归档到 `cache/terrain-world/<时间戳与随机标识>/`，包含 `receipts.jsonl`、`result.json`
和 10 张截图。该目录保持忽略；不提交缓存、购买清单或私有资源。
原生模块、构建与实测平台结果统一见[测试文档](../../../docs/testing.md#大世界地形与-rvt)。

## 制作与限制

制作使用 `start_terrain_authoring` 提交完整 TOML 配方，再查询作业并执行 `publish_terrain_authoring`。
高度图、噪声、印章、画层和道路均写入持久源；运行修改另走异步请求及 checkpoint。
命令、取消、撤销和恢复契约见[地形制作](../../../docs/plugins/editor/terrain-destruction-authoring.md)。
大世界范例聚焦流送与 RVT，不包含建筑坍塌的真实资源验收；建筑与支撑联动使用
[综合场景](destruction-integration.md)及所属原生测试。
专用植被散布规则、二进制制作源块 delta 和非 Metal 平台的实机画面验收尚未完成。
