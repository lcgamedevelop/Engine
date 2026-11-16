# 体积地形插件

运行时 `runtime/terrain` 管理稀疏三维密度、材质层、分区驻留、LOD、挖填和土石存档；
`editor/terrain` 负责生成源、导入、制作作业和 MCP。两者通过标准资源目录使用独立 GUID 索引与块，
运行时不解析 TOML、不链接噪声生成器。架构入口见[地形与 RVT](terrain-world-design.md)。

## 数据与组件

`.terrain` 使用 schema_version=2 的 TOML，保存 bounds_min/max、origin、voxel_size、block_cells、
lod_count、固定 seed、生成 recipe 与有序 edits。运行索引 ETW2 及块 ETB2 只有当前一种格式；
旧描述和旧存档明确拒绝，项目源应直接升级并重建缓存。每块 8³、16³ 或 32³ 样本；
索引最多 65536 个块，空气和实体可只存一个样本，混合块通过 stream_dependencies 按需读取。

普通节点添加 TerrainComponent，显式设置 terrain_guid、streaming_enabled、collision_enabled、
rvt_role、initial_view 和 budget。可选开关默认关闭。地形保持正等比缩放；
分区键属于地形局部坐标，SetView、笔刷与查询 bounds 使用世界坐标。

TerrainVolume 持有已驻留的不可变块和修改索引；TerrainRegionPlanner 选择视域、碰撞窗口、LOD 和退出区域，
TerrainMesher 使用 Transvoxel regular/transition 单元。宿主注入规划与网格实现，
业务和 MCP 只调用 TerrainComponent，不向下转型。生成节点和 MeshRenderer/Collider 均为瞬态。

## 操作与完成

C++ request_edit/collapse/reset/retire/save/load 返回包含世界代次、节点、组件实例及序号的操作 ID。
TS 对应 RequestEdit/RequestCollapse/RequestReset/RequestRetire/RequestSave/RequestLoad。
GetOperation 只查询；ReleaseOperation 释放终态记录，CancelOperation 只取消尚未发布的工作。
没有同步兼容接口或另名 Async 分支。参数、返回状态及错误见[操作类图](class-diagrams/terrain-operations.md)。

任务由现有 TaskSystem 执行；候选使用不可变输入，在协调线程校验所有者与版本，
经核心 GeometryUpdate 同批切换显示、碰撞和版本。失败保留旧状态，预算不足有明确状态。
初始场景通过 ScenePreparation 扩展 GameplaySession 的候选准备，再进入原有封存/激活流程。
SetView/RemoveView 控制临时需求，PinRegions 保留物理或业务需要的区域；查询支持分页。

基础源与运行修改分离。RequestSave 保存完整修改索引、不可变载荷及动态土石的姿态、速度、睡眠和 retired 身份；
RequestLoad 校验源 GUID/hash 后准备候选，失败不拆掉活动世界。文件采用先写载荷、最后原子替换清单，
保存范围包含已退出视域的修改。槽内路径拒绝越界和符号链接。详见[存档与破坏](class-diagrams/terrain-world-state.md)。

地形与建筑插件不互相依赖。业务等待地形 published/completed，再 CollectSupport，
把完整静态 Collider 集合交给 DestructionComponent.RefreshSupport；未知或过期区域不能当作空地。
导航、音效、粒子及整体游戏 checkpoint 由调用方组织。

## 材质与 RVT

源 material_layers 引用普通 PBR 材质。编辑器编译 BaseColor、Normal、ORM 数组和参数调色板，
运行时只加载合成材质及其实际依赖；每个三角形保留四个主要层与插值权重，地表和洞壁使用三向投射。
当前层输入为 opaque、metallic-roughness 的 builtin PBR；支持颜色、金属度、粗糙度、法线强度、UV 和常量自发光。
Hex tiling、透明/裁切及自发光纹理在层编译时明确拒绝。输入纹理为 Basis 或 RGBA8，当前合成层为 128×128、完整 mip 链。
这不是任意项目 Shader 的通用离线烘焙器。

RVT 只缓存地表材质，唯一运行开关是 EnvironmentComponent 的 rvt.enabled。
洞壁/地下按高度和法线判定回到直接 PBR；关闭 RVT 释放页资源，源体积与材质仍可正常使用。
查看器是临时诊断开关，与生产开关独立，见[RVT 类图](../../runtime/class-diagrams/runtime-virtual-texture.md)。

## 验证与范围

公开 64 米实验场验证 1 米采样、洞穴与跨块编辑；8×8 公里世界用 16 米采样验证流送和四角切换，
不把低分辨率标准世界当作高密度整图性能结果。具体操作和截图入口见[showcase](../../../projects/showcase/docs/terrain-world.md)，
编译、测试及平台范围统一见[测试文档](../../testing.md#大世界地形与-rvt)。

树木/建筑仍为独立普通节点或分区场景，由项目装配；专用生物群落散布规则与可独立编辑的二进制源块 delta 尚未提供。
当前制作 edits 是有序持久配方；运行修改存档已经采用分块载荷。全球导航、任意无限世界和多层体积 RVT 不在当前能力内。
