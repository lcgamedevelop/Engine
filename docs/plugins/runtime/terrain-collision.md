# 地形几何与碰撞更新

地形按驻留区域生成独立瞬态 MeshRenderer 与静态 Collider，复用核心 MeshCollisionData、Jolt 和 GeometryUpdate。
本页维护当前分区实现；历史单体地形的计时数据不能套用于分区后的行为。

- LOD 只控制显示，物理窗口使用 LOD 0 的权威密度；过渡缝追加显示拓扑，不替换权威碰撞三角形。
- 体素修改收齐拥有者、梯度和相邻过渡区域，后台生成不可变网格。只重建变化块和其必要邻域，GPU 不重新上传整张世界。
- GeometryUpdate 先准备渲染资源与 Jolt 候选，校验世界、场景、节点及组件实例身份后同批发布；失败和取消保留旧画面与旧碰撞。
- 新节点通过候选 scope 准备，退出区域在同批完成资源替换后释放；不能在 GPU 完成前显示新的碰撞版本。
- 角色和动态体依赖的区域由显式视域和 pin 保留。collect_support 只有全部查询区域的当前碰撞完整时才成功，已知空气与未知数据严格区分。
- 动态土石使用独立 Node、MeshRenderer、Collider 和 RigidBody。连通搜索不足返回 connectivity_incomplete，不能把未加载窗口外当作空气。
- 静态密度移除、动态碎块创建及碰撞切换属于同一候选；存档保存碎块自身的密度载荷和稳定身份，不从已挖空的源块推测。
- Jolt 双精度位置用于世界放置；局部形状顶点继续 float。核心不认识地形区域键，不包含插件策略。

通用事务见[GeometryUpdate](../../runtime/class-diagrams/geometry-update.md)，
分区及算法见[TerrainVolume/Mesher/Planner](class-diagrams/terrain-regions.md)，
存档、支撑和生命周期见[地形世界状态](class-diagrams/terrain-world-state.md)。
测试采用现有 terrain、physics、navigation 模块用例与公开场景 MCP 流程，记录见[测试文档](../../testing.md)。

当前固定依赖为 Jolt 5.6.0（MIT）和 Transvoxel `51a494f03c5b024cd153b596bcc7152eb3cc93a6`（MIT）。
Jolt 的 double precision 在本地 vcpkg overlay 目标中开启，不修改上游检出源码。
