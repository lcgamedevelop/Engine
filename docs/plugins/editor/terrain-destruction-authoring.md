# 地形与建筑制作

地形与建筑分别由 editor/terrain 和 editor/destruction 插件实现；MCP 转发现有 EditorCommands，
运行时只消费标准 GUID 资源。地形制作与建筑预破碎是独立流程，不共享一个通用 Manager。

## 地形制作流程

1. get_terrain_capabilities 查询功能与限额，get_terrain_source 读取源版本。
2. start_terrain_authoring 提交 path、expectedSourceRevision（十进制字符串）和完整 TOML source。
   创建时预期版本为 0，新 source.revision 必须是预期值加 1。
3. get_terrain_authoring 查询 queued/preparing/ready_to_publish；准备期间不改变正式源。
4. publish_terrain_authoring 把候选交给现有撤销命令，校验路径、旧源和 GUID 后发布源/meta。
5. 通过正常资源导入/预览刷新生成当前目录，等待区域与碰撞就绪，再截图或运行；published 不代表 GPU 已完成。

取消和释放使用原会话/序号。相同请求复用作业；新会话重新提交完全相同的配方时，
准备 journal 可恢复，已发布源被识别为 published，不重写文件或换 GUID。
旧会话 ID 明确失效。源已被其他操作修改时撤销失败，不覆盖外部改动。

配方支持 FastNoise2 图、高度图样本、球/盒印章、挖填/平滑/平整/材质笔刷，以及带宽度与 falloff 的道路控制折线。
有序 edits 保存在 .terrain 源。每块采用局部配方指纹，远处笔刷不重算无关块；生成缓存位于 cache/terrain-authoring，
运行产物仍只位于 cache/gen/GUID。生成器实现或规则变化须更新生成指纹，不复用其他算法的旧缓存。
平滑计算限制递归深度为 32、临时采样记录为 262,144；超出时作业失败并保留已发布源，
不在自动化请求中无限展开平滑叠加。源字段类型错误会明确拒绝，不替换成默认参数。
材质数组及当前限制见[运行时地形](../runtime/terrain.md#材质与-rvt)。

源文件可撤销，运行挖掘使用 terrain_request_edit 等异步原生请求并保存到 App 持久槽；
运行修改不会偷偷改制作源。普通节点、模型、预制体和分区场景使用既有编辑命令；
专用散布规则与二进制可编辑源块尚未实现，不能把运行存档当作制作源导入器。

## 建筑破坏资源制作

- `.destruction` TOML 描述 `size`、`fracture = "slices" | "voronoi"`、`slices_x/y/z`（每轴最终份数）、`sites`、`seed`、`bond_health` 与 `anchor_height`。原始盒体底部位于局部 y=0，节点变换决定场景位置；未知字段、超限或无效值明确失败。
- 切割、三角体积积分和 EXACT 三角面接触连接使用 Blast 5.1 官方 ExtAuthoring；最多 256 个块，尺寸各轴不超过 1000，连接强度大于 0 且不超过 1e6。当前只支持参数盒体，不将生成试件宣称为任意 FBX、空心建筑或实时切割工具。
- 生成 `DestructionAssetData` 保存片段几何、体积、质心、初始锚点与连接；初始锚点强度使用 `bond_health`。不发布 Blast 平台内存布局，材质由场景组件显式引用。
- `DestructionImporter` 参与现有增量管线和原子资源发布；`DestructionCodec` 维护 `type = "destruction"` 与 destruction/material GUID、密度、摩擦及恢复系数。运行时不需要 TOML、Authoring 或 Boost。
- 动态组使用 Jolt 官方凸包/复合形状，一个组可保留多个相连碎块。首版仅允许根节点与正等比缩放；显示和碰撞节点都是可重建的瞬态对象，不进入场景持久数据。
- 切面独立材质、碎块层级和任意模型输入尚未实现。运行时应力、地基采样和撞击反馈已接入，使用相同破坏资源；制作工具不承担运行时求解。

## 界面、诊断与自动化

set_terrain/get_terrain 只操作真实组件。terrain_set_view、terrain_query_regions、terrain_request_*、
terrain_operation_status/cancel_operation/release_operation 复用同一运行时接口；不另设 AI 算法。
所有节点工具显式传 sceneId/nodeId，状态和操作 ID 的 uint64 字段为十进制字符串。

RVT 由环境组件全局启停，set_render_feature(feature="rvt_viewer") 只控制实时面板。
set/get_rvt_debug_options 控制 overview/virtual_texture/physical_atlas/page_table 与颜色、法线、粗糙度等通道。
面板采样真实 GPU 纹理，截图含实际帧与 RVT 版本；不逐帧回读到 CPU，也不扩大页面需求。

脚本测试可在显式测试构建的当前 Run 内 execute_project_script；正式包不装配测试入口。
制作类图见[地形编辑器](class-diagrams/terrain-editor.md)，项目完整操作和归档见[showcase](../../../projects/showcase/docs/terrain-world.md)。
