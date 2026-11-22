# 测试与验证

只验证当前修改直接影响的行为。日常使用少量基础检查和现有模块用例；不因一次修复扩展全仓测试，不将性能采样当作每次提交的必经步骤。

## 按修改范围验证

| 修改 | 必要验证 |
| --- | --- |
| 文档、类图措辞 | 改动页的本地链接、围栏与 `git diff --check`；不构建引擎 |
| CMake、测试登记 | 对受影响配置重新生成，查看 `ctest -N` 和目标；不因此运行全部测试 |
| Schema 分层与生成 | 按层生成 C++ / TypeScript，检查目标依赖方向，选择对应导入及跨语言用例 |
| C++ 实现 | 受影响目标 Debug / Release 构建；仅运行覆盖该行为的 Release 用例 |
| 脚本或资源 | 对应项目的类型检查、导入或实际使用路径，复用已有入口 |
| 渲染行为 | 选择一个相关的现有 GPU 用例；只有改到窗口、设备或多效果生命周期时才运行 `engine.window_render` |
| 性能优化 | 一个代表场景、一对同条件前后数据；出现波动、失败或结论不足时才追加采样 |

一次修改开始时选定最小验证集合；通过后不因整理文档、准备提交而重复执行。新增用例必须能捕获现有测试缺少的真实契约或回归；优先补入所属模块现有测试，不为每个修改点再建测试程序。不重复验证第三方库本身，不维护临时场景的全排列矩阵。

## Android 项目出包

`engine.android_asset_packaging` 验证项目省略显示和 SDK 选项时传给 Gradle 的生效值、资产目录和原生包的交付约束，也验证快速重打包发现其他项目的资源清单或运行设置时会保留上次导出的 APK。`editor.google_play_publishing` 覆盖简版与自定义版本规则、显式覆盖全屏和目标 API、签名临时文件的隔离与清理、APK 和 AAB 选择、离线构建及商店事务恢复。商店事务用例使用模拟响应。[项目设置与编辑器命令](plugins/editor/google-play.md)

Android 14 ARM64 模拟器上，省略 `fullscreen` 的横屏游戏和竖屏公开范例，从第一个 SDL Surface 到首个 Vulkan 画面均分别为 1280 × 720 和 720 × 1280。退到桌面再恢复时，应用窗口仍占满显示区域，状态栏和导航栏均处于隐藏状态。显式关闭全屏的竖屏应用使用目标 API 34、编译 API 36，首帧为 720 × 1108，两种系统栏均可见。复核启动尺寸时，从冷启动日志依次读取 SDL 的 `Window size` 和 `Renderer initialized: Vulkan`，再用 `dumpsys window displays` 检查系统栏的实际可见性。

## 大世界地形与 RVT

当前实现与边界见[地形架构](plugins/runtime/terrain-world-design.md)，
公开场景和复现命令见 [showcase 地形文档](../projects/showcase/docs/terrain-world.md)。
macOS arm64 / Metal 已完成受影响 Debug、Release 编译，功能测试只使用 Release。
本轮验证属于地形及直接依赖链，不代表全引擎或所有平台通过。

| 已覆盖内容 | 证据入口 |
| --- | --- |
| 稀疏块、LOD 过渡、区域预算、未知邻区拒绝、取消、显示与碰撞同版本发布；禁用节点和祖先不阻塞准备；当前需求失败与恢复 | `engine.terrain_runtime` |
| 源格式、噪声/高度图/道路/画层、局部块缓存、层纹理编译、可撤销源事务与重启恢复；宿主生成器注入与实现指纹隔离 | `editor.terrain_pipeline` |
| 流送依赖随包导出而不自动常驻、目录版本与失败原子性 | `engine.resource_catalog`、`plugins.runtime.resource_graph`、`editor.asset_pipeline`、`editor.asset_build` |
| 场景准备、几何事务、ECS 生命周期 | `engine.scene`、`engine.runtime_host` |
| 双精度位置、睡眠状态、局部导航与骨骼世界姿态 | `engine.physics`、`engine.navigation`、`engine.navigation_terrain`、`engine.animation_rigging_events` |
| 破坏支撑集合、建筑候选分批准备与取消、封存后到激活前的隔离、异步地形脚本、存档与碎块 | `engine.destruction_runtime`、`engine.terrain_destruction_script`、`engine.terrain_runtime` |
| RVT 页面生成、材质回退、开关及实时 GPU 面板；无相机帧不沿用缓存的生效状态 | `engine.terrain_runtime` 及公开项目 MCP 实窗回执 |
| 新纹理数组编码、项目类型与用例登记 | `resource.codecs`、showcase `case_contract_test.mjs` |
| Shader 依赖、跨语言 RVT 数据与 MCP 工具登记 | `editor.texture_pipeline`、`engine.render_foundation`、`engine.scene_data`、`engine.scene_schema_cross_language`、`engine.script_tooling` |
| 依赖方向和源码约束 | `engine.architecture_boundaries`、`engine.architecture_rules` |

当前修复的 7 项 Release 检查通过：`engine.terrain_runtime`、`editor.terrain_pipeline`、
`engine.terrain_destruction_script`、`engine.render_submission`、`engine.script_tooling` 及两项架构检查。
脚本回归包含 RVT 数字枚举及非法输入拒绝；状态查询同时校验当前与已发布需求身份、失败错误及预算恢复。
公共交付工具的 Android 30 项、桌面/iOS/Web 28 项测试通过；目录夹具从当前 Schema 生成，
覆盖 terrain_block、流送引用、旧版本拒绝及失败时保留既有输出。
真实 macOS 运行包包含 4,205 项资源与两个合并包，已通过公共交付工具的复制、实体头和逐项哈希校验。
这些交付检查不代表 Android、iOS 或浏览器实际运行通过。

`engine.plugin_selection` 通过。另以同一 Release 预设关闭 runtime/editor 插件与测试支持，
确认不生成地形、FastNoise、MCP、脚本或编辑器目标；核心宿主实际链接并通过 Metal 绘制 4 帧后正常退出。
运行设置使用项目当前源汇总，旧版本运行设置明确拒绝。

公开 8×8 公里世界使用 16 米采样、固定种子；1 米实验场保留道路、洞穴与层混合。
MCP 实窗流程已完成四角切换、挖地、保存、重置、恢复、法线通道、生产 RVT 关闭和查看器关闭，
共 10 张真实截图。四角均等待当前需求页面就绪；关闭 RVT 后 `residentBytes=0`，关闭查看器后
截图帧 `rvtDebugVisible=false`。归档位于
`projects/showcase/cache/terrain-world/20261116T074006Z-91663412/`，包含帧证据与操作回执。

定向实窗回归归档在 `projects/showcase/cache/branch-review-fixed/live-corrected/`，包含 4 张截图和 MCP 回执。
禁用地形节点的追加批次正常到达 ready；将活动地形预算调低后返回 failed / planning_failed，
旧发布版本与画面保留，恢复预算后发布身份与当前需求再次一致。
禁用相机后仍保留 RVT 页面缓存，但 effective=false、meshDraws=0、截图 rvtReady=false；
恢复相机后页面重新生效。运行时业务、脚本与 MCP 读取同一状态，没有额外测试加载流程。

公开资源不含付费或私有内容。没有运行 Windows、Android、iOS、WebGPU 实机地形验收；
四角截图也不等于已经验证所有位置的角色、音频、粒子和碰撞组合。
专用植被散布规则与二进制制作源块 delta 未实现；当前使用持久配方、有序 edits 和局部生成缓存。
私有真实资源项目未列入这轮验收。截图、缓存和运行存档继续保持忽略。

## 执行方式

构建环境与平台预设见[构建指南](building.md)。普通构建只默认编译 `engine_framework_test`、`engine_guid_map_test`、`engine_system_test`；Release 测试预设默认也只选择这三项。其他测试仍保留独立目标，由调用方按模块显式选择，不随普通引擎构建全部编译。

```powershell
# 基础检查：已有构建不必重复构建。
cmake --build --preset windows-static-release --target engine_framework_test engine_guid_map_test engine_system_test
ctest --preset windows-static-release

# 模块检查示例：-R 替换预设的默认集合，不额外叠加基础检查。
cmake --build --preset windows-static-debug --target engine_render_submission_test
cmake --build --preset windows-static-release --target engine_render_submission_test
ctest --preset windows-static-release -R '^engine\.render_submission$'

# 只查看选中的用例，不执行。
ctest --preset windows-static-release -R '^engine\.render_submission$' -N
```

其他平台使用对应 Release 预设。Debug 只编译，已移除默认 Debug 测试预设；只有用户明确要求时才直接指定 Debug 构建目录运行测试。GPU 用例仍须配置 `ENGINE_ENABLE_GPU_TESTS=ON`，需要真实桌面与图形设备，不以 Noop/headless 结果替代画面验收。

全量构建/运行仅用于明确要求的完整验收，不是日常流程：

```powershell
cmake --build --preset windows-static-release
cmake --build --preset windows-static-release --target engine_tests
ctest --preset windows-static-release -R '.*'
```

CI 仍仅手动触发，默认执行基础集合；跨平台或完整功能验收须明确范围。不能把默认三项通过报告成全引擎通过。

### 测试目录

| 目录 | 归属 |
| --- | --- |
| `tests/runtime/<module>/` | 核心、资源、场景、渲染等运行时模块 |
| `tests/editor/<module>/` | 编辑器宿主、项目、命令、场景保存和资源管线 |
| `tests/plugins/runtime/<plugin>/` | 资源图、脚本及其他运行时插件 |
| `tests/plugins/editor/<plugin>/` | MCP、建模、导入、烘焙和构建同步 |
| `tests/build/` | 构建、插件选择、交付与架构规则 |
| `tests/support/`、`tests/fixtures/` | 共用夹具 |
| `projects/showcase/tests/` | 公开项目自身的行为 |

可视测试应保留实际画面，方便复现与检查。`engine.window_render` 每次运行自动将原始 GPU 截图保存到构建目录的 `test-artifacts/window-render/<时间戳>/`；设置 `ENGINE_RENDER_TEST_ARTIFACTS` 可指定已存在的归档根目录，每次运行仍创建独立时间戳子目录；指定目录不可用时使用 `ENGINE_RENDER_TEST_ARTIFACTS_FALLBACK` 指定的已存在备用根目录；两者均不可用时失败。`engine.font_ui_render` 默认保存到测试工作目录的 `test-artifacts/font-ui/<时间戳>/`，可用第二个命令行参数指定目录；临时源项目销毁时保留这些图。公开范例的菜单场景及交互截图入口见 [showcase 文档](../projects/showcase/docs/index.md)。纯逻辑测试没有画面，不生成截图。

登记入口为 [tests/CMakeLists.txt](../tests/CMakeLists.txt)。不为测试修改正式接口可见性，不提交运行结果或生成资源。

## 文件操作与通用资源读取

`engine.script_runtime` 在 QuickJS 与 V8 共用同一用例，覆盖 File 的活动回调限制、文本和字节读写、Uint8Array 子视图及独立副本、空文件、非法编码、共享／已分离缓冲区拒绝、目录枚举和文件变更；Resources 覆盖 source 资源读取、释放后的副本、类型与句柄校验及目录切换。`engine.resource_read` 补充符号链接与失败输出状态，`editor.asset_pipeline` 覆盖通用源文件原样导入及增量复用。

macOS arm64 已通过受影响目标的 Debug / Release 构建；Release 的上述三个用例以及 `engine.resource_catalog`、`resource.codecs`、`plugins.runtime.resource_graph`、`engine.asset_sync` 共 7 项通过，V8 的 `engine.script_runtime` 也通过。架构边界与规则两项检查通过，Android 资源打包脚本的 29 项测试通过，其中包括拒绝旧目录／实体格式并保留已有输出。打包脚本检查不代表 Android 真机验收；文件操作尚未完成其他平台的实机验证。

公开范例的资源与脚本升级结果见 [showcase 验证入口](../projects/showcase/docs/index.md#验证)；私有项目的调用方、meta、测试与构建记录留在其本地项目目录。

## 大体积 FBX 导入

原生 FBX 使用显式 2 GiB 输入预算，同一字节快照用于哈希和 ufbx 解析；普通整文件读取仍默认限制为
64 MiB。源文件校验使用分块哈希，发布前对每个源及 meta 各读取一次，再逐项核对所有子资源的快照。
共用 MikkTSpace 生成器保留有效逐角结果，退化切线从副切线恢复；完全退化时生成确定的正交基，
不覆盖源法线、UV 或材质。契约见 [FBX 导入类图](plugins/editor/class-diagrams/fbx-importer.md)和
[资源管线](editor/class-diagrams/asset-pipeline-system.md)。

macOS arm64 的受影响 Debug / Release 目标编译通过；Release 的 `engine.resource_read`、
`editor.fbx_bind_pose`、`editor.fbx_pipeline`、`editor.asset_pipeline` 通过。覆盖默认预算拒绝与显式
预算读取、超过 2 GiB 提前失败、大于 64 MiB 的有效 FBX 完整发现/转换/增量/发布，以及退化切线的
单位正交性和源法线保留。输入损坏、非法法线及源/meta 变化仍拒绝发布。

本机 Metal 编辑器已用 Bistro 原始外景（119,845,216 字节）及室内模型完成导入，并在同一运行实例中
切换场景、设置相机和捕获真实场景帧。FBX 解析树在全批转换完成后释放；运行实例由实际组件保持独立
资源引用，启动代码不额外固定持有首个模型。原始贴图的颜色、法线、材质槽等项目适配与结果集中在本地
项目自身文档，公开仓库只维护通用引擎契约。本地测试项目、源包和归档保持忽略；结果不外推其他平台。

## 手动集成与 GPU 验证

- `engine.render_submission`：剔除、实例化、材质/Probe 分组、LOD、瞬态缓冲不足和阴影像素回归。阴影覆盖实际提交减少、实心和裁切投影、相机及投影物变化后的重绘、动态变形，以及两个平面反射中方向光和点光独立刷新与 HDR 像素还原。开启 SSR 后验证材质表面提交，支持 4× MSAA 时还验证不透明深度预通道的设备能力、共面遮挡、alpha 裁切和颜色附件复用的逐像素 HDR 对照。关闭 SSR 后检查通道退出及图像恢复。另覆盖两个平面反射、CSM 与 Meshlet 组合。
- `engine.scene_hdr_capture` / `engine.scene_hdr_transparency` / `engine.hdr_extension_capture`：按实际修改选择对应捕获或透明链路。
- `engine.window_render`：窗口、GPU 资源、多效果与生命周期集成。关闭 SSR 的夹具按深度和不透明颜色通道校验绘制次数。预览扩展用相机旋转校验视图矩阵，用 `world_origin` 校验平移。耗时较长，不作为所有渲染修改的默认补充项。
- `editor.scene_bake_core_loader`：实际 GPU IBL、纹理捕获、多面立方体 UV2 与跨帧照明积分；连续取走结果后不重新准备就渲染时必须延后读回，准备完成后再返回独立、非空的像素结果，覆盖编辑器逐面烘焙的调用顺序和 lightmapper 借用输入的生命周期。同一用例还以单工作项和 256 字节上传预算重绑新的 Lightmap，验证 GPU 上传未完成时帧继续提交、最终就绪并可清除。
- `editor.effects_pipeline`：正式粒子资源及依赖导入、增量和失败原子性；场景经通用保存器写出后须能重新导入，并保留特效组件的启用、可见性状态。
- `tests/plugins/editor/scene_bake/` 的独立 Python GPU 脚本：仅在修改相应烘焙算法时选取，不全部执行。
- `engine.plugin_selection`、架构规则、平台打包、V8 缓存与工具链检查：仅在对应构建/装配修改时运行；V8 检查仅登记于 V8 配置。

长时间运行、截图、MCP 交互和基准采样由明确任务触发，不加入默认 CTest。真实 UI 操作不能用直接调用脚本回调代替，源码或宿主替身检查也不能宣称真机/像素通过。

## 场景加载与运行态测试

业务和测试共用 GameplaySession 的原生场景请求、Flecs 世界、资源图及渲染缓存。测试等待 staged、ready、active 和目标范围的实际场景帧；不会把资源读取结束、窗口有帧或图片非黑单独当作加载成功。动态批次用例同时检查旧场景仍提交、发布前没有该批次的帧证据，发布后内容版本递增。

运行态 MCP 与临时脚本注入只在 `ENGINE_ENABLE_TEST_SUPPORT=ON` 的独立测试构建装配。Debug 只编译；Release 运行所属场景、脚本、渲染、宿主与 MCP 用例。正式构建额外检查解析后的目标、源码与链接依赖，以及最终程序符号，不以“没有启动服务”代替编译隔离。预设见[构建指南](building.md)，端点和调用方式见[MCP](plugins/editor/mcp.md#独立运行程序测试)。

原生像素用例先等待同一个渲染准备接口就绪，再受理新帧截图；需要检查 GPU 存量时还须等待回读纹理的延迟销毁。重复步骤使用不同文件名，禁止覆盖旧图来冒充新截图。项目实窗用例按 captureId 等待读回及原子发布，并保存实际帧身份、请求回执和状态。黑色可以是合法画面，断言由具体用例决定；菜单用例还必须出现实际 UI 绘制。

2026-11-11，在 macOS 27.0.1 / arm64 / Metal / QuickJS 上完成受影响目标的 Debug、Release 编译和链接；Debug 未运行测试。最终代码完整执行 142 项 Release CTest，142/142 通过，耗时 367.15 秒。覆盖启动请求的 preparing/ready/active 边界、纯查询及终态释放、主动停止与故障的结果保留、重启不覆盖历史错误；同时验证按目标排序发布、排队取消、替换截止、目标卸载、首帧后激活，以及准备期间新增旧对象和先前追加批次的退休、常驻子树保留。注入实际渲染失败后，宿主持续报告 failed，在途请求保留错误，不以清屏替代成功。

所属现有用例补充了四项回归：物理候选对象在发布和激活前不参与模拟；空活动场景的特效清理不重置候选准备进度；已退休镜头丢弃参数桥接记录，当前场景禁用适配器仍恢复默认值；输入回调故障在安全点锁存错误并结束在途请求。四项均在修复前复现失败、修复后通过，负向对照单独归档，不计入当前失败。MCP 关闭前重新采样原生状态，末帧故障不受平时状态写入节流影响。

窗口用例归档 169 张 PNG，字体/UI 用例归档 13 张 PNG，公开范例 14 个菜单入口各保留一张目标场景帧；真实 UI 交互另有 3 张、独立运行态 MCP 有 1 张，共 200 张实际画面。原始 CTest 报告、按用例合并的最新结果、图像清单、指纹与自包含预览位于忽略的 `build/test-audit/<run>/`，原图仍在各测试或项目缓存目录，`run.json` / 项目结果保留配置和归属信息。纯逻辑用例没有图形输出，不生成截图。

公开项目实际交互检查启动 prepareId 与 Run 归属、终态释放不影响玩法、同一 Gameplay/Model 的临时脚本、停止重启后的旧身份拒绝、菜单点击、文字输入与跨场景 UI 保留；独立测试运行包验证相同启动结果契约、临时脚本、UI 场景截图及正常退出。全部菜单入口完成进入、目标范围截图和返回，采集方法见[范例验证入口](../projects/showcase/docs/index.md#验证)，截图归档使用独立记录根目录的 `records/validation/showcase/`；采样和目录配置见[性能采样](runtime/performance.md)。

当前方向光阴影连续性及材质/反射像素回归通过；抽查原图并浏览全部联系表，未发现本轮加载变更引入的黑屏。SSR 对照夹具仍有阶梯状反射条带，相关 PNG 与此前已审阅图逐字节一致；现有用例检查反射命中和材质权重，不代表反射质量已经完整验收。该限制保留在当前记录，本轮不扩展反射算法。

正式 Release 配置未创建 MCP 或编辑器目标，解析后的源码及链接图不含 MCP；最终程序没有临时脚本执行、运行端点或命令通道符号，独立测试产物包含对应符号。以上只证明本机配置及所列行为，不包含 Windows、Android、iOS、V8、较旧 macOS、所有私有项目场景或完整冷启动性能。私有项目的 5 项脚本检查及白模、动画实验室连续进入、截图、暂停和恢复已通过，项目结果保存在其忽略的缓存目录。

预算是工作项之间的软上限，第三方调用与 GPU 管线创建仍可能形成长帧，不能据此承诺任意场景零卡顿。编辑器发布前的源文件校验契约及大体积模型回归见上文；该阶段发生在玩法启动前。

## 公开范例

公开项目入口、场景、资源来源和操作说明统一在 [showcase 文档](../projects/showcase/docs/index.md)。引擎文档不再重复维护逐场景节点数量、操作长表和每次验收日志。

[Meshlet 标准场景](../projects/showcase/docs/meshlet-culling.md)提供公开几何、运行开关及可复现的截图检查；当前验证结果见下节。

通过编辑器启动、MCP 控制和退出的操作见[编辑器](editor/editor.md)与[MCP](plugins/editor/mcp.md)。运行前核对项目、运行目录和编辑器会话；只关闭本次测试拥有的进程。脚本后端、资源目录、分辨率和设备必须在结果中说明。

## Meshlet 与 GPU 剔除

2026-11-15 至 16 日，macOS arm64 / Metal / QuickJS，Ninja 与 ccache。受影响目标的 Debug、Release 编译和链接通过，Debug 未运行测试。Release 的 9 项模块检查通过：`editor.mesh_lod`、`editor.mesh_lod_pipeline`、`editor.scene_persistence`、`resource.codecs`、`engine.render_submission`、`engine.runtime_settings`、`editor.runtime_settings`、`engine.architecture_rules`、`engine.architecture_boundaries`。本项没有重跑无关模块或全量测试。

离线检查覆盖多材质和各级 LOD 的生成、原三角形顺序、精确拓扑校验、稳定 GUID、无变更不重建及关闭生成后移除附加数据。原生 Metal 渲染用例检查普通／间接路径的严格线性 HDR 对照，以及 GPU 数据尚未准备时保持普通帧、预算不足整批回退、LOD、镜像、材质、探针和阴影；像素对照通过，最大差值为 0。

`editor.mesh_lod` 另覆盖全部退化的簇、退化与正常面混合以及大坐标下的小簇：保持源索引，复用上游球体计算并向外舍入半径，不放宽校验。回归在修复前分别失败，修复后与 `editor.mesh_lod_pipeline` 同时通过。

公开范例的 15 项菜单配置检查及 Meshlet 实窗用例通过。实窗使用 960×640 逻辑窗口、1920×1280 后缓冲和正常导入缓存，实际点击场景按钮与开关，并用编辑器 MCP 查询同一运行实例。透视、正交两组截图排除顶部 UI 后均逐像素一致，最大通道差值为 0；临时边界视图、GPU 统计、透明与不适用材质的普通路径、返回菜单和重入默认值均通过。该固定透视镜头的一次已完成 GPU 样本为 1,008 个候选 Meshlet／实例组合、895 个保留组合，候选三角形 56,976、保留三角形 49,925；这是提交内容统计，不是帧率或耗时收益。

`meshlet_colors` 的原生渲染与公开实窗检查通过：实际表面以稳定的逐簇色块显示，场景剔除关闭时仍可单独开启，与包围线可组合；连续帧及关闭后 PBR 恢复的最大通道差值均为 0。公开用例还将 18 个球体和 4 个拱门切换为普通绘制，与实例化色块核对表面覆盖，未发现漏色或超出容差的边界像素。调试绘制沿用主通道实际采用的顶点变换输入及顺序，不通过放宽深度比较或深度偏移覆盖误差。

私有真实资源沿同一导入和渲染链路检查。等待普通画面连续稳定后，普通／Meshlet／关闭后恢复三张完整截图逐像素一致，最大通道差值为 0。独立色块视图也完成稳定帧截图，关闭后完整 PBR 截图逐像素恢复；色块启停没有改变场景剔除开关。该镜头未开启视锥剔除，因此本次只证明真实资源的路径与画面一致性，不将其用于视锥剔除效果或性能结论；公开用例另有透视、正交视锥覆盖。私有资产、源 meta、回执及截图全部留在私有项目自身的忽略目录。

公开采集及回执写入记录根目录的 `records/validation/showcase/visual-tests/meshlet-culling/<run-id>/`。私有项目通过独立记录目录保存结果。操作入口和夹具说明见[项目文档](../projects/showcase/docs/meshlet-culling.md)。上述画面和功能验证覆盖本机 Metal。运行包使用由当前源资源生成的格式。

## 方向光虚拟阴影

macOS arm64 / Metal / QuickJS，使用根预设派生的 Ninja、ccache 配置。受影响目标的 Debug、Release 编译和链接通过，Debug 未运行测试。Release 的 11 项相关检查通过：`editor.commands`、`engine.render_foundation`、`engine.scene_data`、`engine.scene_schema_cross_language`、`editor.scene_persistence`、`engine.architecture_boundaries`、`engine.architecture_rules`、`resource.codecs`、`engine.runtime_settings`、`editor.runtime_settings`、`engine.render_submission`。没有重跑全量用例。

CPU 检查覆盖固定页预算、未发布批次取消、过期反馈拒绝、镜头移动复用、投影物旧/新范围失效和代次隔离；组件与编辑检查覆盖方向光限制、参数原子校验、序列化及精确撤销/重做。Metal 用例验证 GPU 需求反馈、静态缓存的严格 HDR 像素一致性、物体移动后的更新，以及关闭后恢复原 CSM 画面和释放缓存。镜头禁用时的缓存清理回归在修复前复现失败、修复后通过；恢复镜头建立新代次，重建的阴影画面逐像素一致。Scene／Prefab 使用版本 6；旧二进制不兼容读取，项目源资源经现有导入链重新生成。

公开标准场景通过实际菜单进入，并用同一运行实例的 MCP 检查静态零重绘、移动箱体、六种调试模式、非法级别拒绝、关闭/重开和返回菜单清理。归档 10 张 PNG，包含普通画面、移动后画面、虚拟深度/图集/页表等诊断及关闭/恢复对照。固定镜头最终 425 个需求页全部就绪，缺页为 0。真实资源本机验证另归档 3 张截图，覆盖 VSM、实时诊断及 VSM/RVT 同时显示；三张回执均包含实际场景帧的 `vsmReady=true`，RVT 同时开启时也为 ready。私有资源、脚本和截图只保存在私有项目忽略目录。

公开采集写入记录根目录的 `records/validation/showcase/visual-tests/virtual-shadows/<run-id>/`。`result.json` 和 `receipts.jsonl` 保留实际提交帧及 VSM 修订。移动后需等需求页就绪，并在截图回执核对实际使用的 VSM。 [公开 VSM 场景](../projects/showcase/docs/virtual-shadows.md) · [VSM 类图](runtime/class-diagrams/virtual-shadow-maps.md)

6 个受影响 Shader 另通过 SPIR-V 与 WGSL 编译；这不是 Windows／Android／iOS／浏览器的实际运行验收。设备恢复及全部蒙皮、地形变形、破坏组合尚未逐项实机验证。首版限主视图的一盏方向光，透明、反射、烘焙和缺页沿用 CSM；未实现 SMRT、局部灯 VSM 或静动双缓存，也未测量性能收益。

## 镜头与角色模块验证

2026-11-09，macOS arm64 / Metal / QuickJS。使用继承根 `ninja-engine-debug`、`ninja-engine-release` 的本地预设，Ninja 与 ccache；`editor_app`、`engine_app`、`mcp_app`、`asset_build_app` 及受影响模块测试目标的 Debug / Release 构建通过。Debug 仅编译，未运行测试。

Release 的 13 项 CTest 全部通过（9.06 秒）：`engine.character_motor`、`engine.mcp_registry`、`engine.scene`、`engine.prefab`、`engine.plugin_selection`、`engine.architecture_boundaries`、`engine.architecture_rules`、`resource.codecs`、`engine.camera`、`engine.character_controller`、`engine.character_host`、`editor.character_controller`、`engine.control_script`。

相机用例覆盖目标失效、失败不消费触发器、参数原子修改、混合与四元数旋转、约束冲突、碰撞收缩和延迟恢复。角色用例覆盖多实例、固定步动作缓冲与到期、加减速、控制权独占、外力保留、蹲起净空、暂停／禁用／传送、观察增量与速率，以及实际姿态向镜头参数的映射。编辑器与脚本用例覆盖完整设置、撤销、瞬态输入、BigInt 身份、可编辑配置快照、旧字段拒绝和失效句柄。

关闭全部运行时／编辑器插件及脚本运行后，独立 Release 配置中的 `editor_app`、`engine_app` 编译链接通过；未创建 camera、character_controller、script 插件目标，运行宿主未链接相机和角色插件实现。插件选择用例同时检查依赖闭包与条件装配。

公开范例 TypeScript 构建及 `launcher_test.mjs` 的 8 项检查通过。实际编辑器窗口使用 `projects/showcase` 源资源导入缓存，逻辑尺寸 960×540、Metal 后缓冲 1920×1080；通过真实菜单按钮进入角色庭院，确认角色就绪、接地和镜头 ready。调用角色镜头接口切换第一／第三人称，截图确认第一人称隐藏自身身体、保留其他角色与自身阴影；随后恢复第三人称并正常关闭测试编辑器。项目配置、控制方式与复现入口见[角色与镜头范例](../projects/showcase/docs/character-control.md)。

此结果不包含实体手柄操作、移动平台场景验收、Windows／Android／iOS、V8 后端、旧版 macOS 发行验证或性能采样，不代表所有范例均已实窗验收。场景／预制体只接受当前格式，旧二进制需通过当前源描述重新导入和打包。

## 渲染性能基线

当前性能采样与中文报告入口见[性能采样](runtime/performance.md)。macOS arm64 / Metal 的受影响 Debug（Tracy OFF）和 Release（Tracy ON）目标编译、链接通过；Release `engine.render_submission` 及性能分析工具检查通过。工具检查覆盖父子／跨线程计时、缺失自身时间、异步 GPU 源帧、旧结果去重、非法时间戳与中文单文件报告。代表场景实窗采集验证了宿主、资源准备、各提交阶段、bgfx 内部回调及原生视图记录；这不等于首帧加载、独占 GPU Shader 成本或其他平台已测。具体项目资源、截图和采样结果留在各自忽略目录。

无损顶点索引生成、普通阴影按 atlas 共用投影物准备，以及没有角色标记时的镜头过滤优化，完成受影响 Debug／Release 构建。Release `editor.fbx_pipeline`、`editor.fbx_bind_pose`、`engine.character_controller`、`engine.character_host`、`engine.render_submission` 通过；复用用例检查重复角点去重与 UV／切线／蒙皮接缝、材质分区和索引属性、旧镜头过滤清理、阴影／反射／Meshlet 的严格 HDR 对照。中文报告脚本的标签转义及输入数量检查通过。实窗只重测本次代表场景，没有重跑全量场景或其他平台；没有把局部 CPU 降幅宣称为 5–6 ms 的完整帧率验收。

普通阴影 atlas 静态复用完成相同预设的 Debug／Release 编译链接，Release `engine.render_submission` 与 `engine.performance_analysis` 通过。现有渲染用例通过实际 bgfx 提交计数检查缓存命中和各类失效，并检查恢复原输入后严格 HDR 一致；原有反射、VSM 与 Meshlet 组合继续通过。实窗代表场景的普通、Meshlet、关闭后恢复三张 PNG 与修改前逐像素完全相同，稳定采样期间普通阴影提交页数为零；收益适用于输入未变化的 atlas，相机或投影物变化仍按正常路径重绘，不代表动态场景或其他平台有同等性能收益。

Meshlet 剔除分派和单实例连续区间合并通过 macOS Debug／Release 构建、Release `engine.render_submission` 和 Vulkan SPIR-V Shader 编译。回归验证多批次共用一次 Compute、相邻簇共用命令且候选数完整，覆盖多实例压紧、LOD 变化、预算回退和阴影／反射组合的严格 HDR 对照。固定机位三张 PNG 与修改前逐像素一致，实际每通道命令槽由 33217 降至 4748。时间线确认主线程场景提交与渲染线程重叠，Metal 的深度与不透明命令在同一线程串行编码。完整帧中位数降低，该次 P95 未降低。

Metal 时间戳仅记录当前采样区间的有效阶段，采集器断开后停止阶段计数。未连接采集器时，临时 MCP 脚本另行记录玩法帧间隔并核对独立墙钟。时间线、截帧和工作缓冲容量等数据见对应本地报告，实窗结果覆盖本机 Metal 的代表场景。

固定深度的 AO 和 SSR 层级复用由 `engine.render_submission` 校验，覆盖机位及 AO 参数失效、恢复稳定输入后的重复帧，以及切换 SSR 的 HDR 像素对照。3456×2234 原生全屏、4× MSAA、全部用例效果开启的本机对照中，固定机位每帧 Compute 调用由 16 降为 5，绘制由 4683 降为 4671。固定机位的 P50／P95 从 33.68／36.36 ms 降至 30.53／32.54 ms。移动时两个通道重新使用当帧输入，P50 从 51.21 ms 变为 51.36 ms。相同机位截图逐像素一致。

VSM 反馈工作组内归并通过 macOS Debug／Release 构建和 Release `engine.render_submission`，反馈 Shader 通过 Metal、SPIR-V 与 WGSL 编译。沿用上述全开设置的同一移动轨迹，Metal 反馈通道中位数从 3.50 ms 降至 0.90 ms，整帧 P50／P95 从 51.36／51.89 ms 降至 49.68／50.29 ms。回到固定机位后的截图逐像素一致，需求页仍为 400 个且全部就绪。

普通级联按受光范围筛选投射物，继续以原投影和深度范围生成阴影。`engine.window_render` 验证了镜头外投射物仍能遮挡可见受光面，斜面 PCF 阴影边缘回归通过。相同全开移动轨迹的整帧 P50／P95 从 49.68／50.29 ms 降至 47.03／47.57 ms，绘制提交中位数由 17121 降至约 13170。固定参考机位和偏移机位的两组前后截图均逐像素一致。

方向光与局部灯分别编译阴影求值，Metal、SPIR-V 和 WGSL 的不透明、地形与水面 Shader 编译通过。`engine.pbr_hdr_direct_light` 使用共享 Shader 校验方向光、点光和聚光，`engine.window_render` 与 `engine.render_submission` 覆盖阴影和反射。相同全开移动轨迹的主视图不透明片元中位数从 17.92 ms 降至 15.88 ms，整帧 P50／P95 从 47.03／47.57 ms 降至 44.20／44.80 ms。静止偏移机位的整帧 P50 从 30.36 ms 降至 27.67 ms，两组对照截图的 RGBA 像素一致。

平面反射复用 MSAA 深度模板，未被采样的颜色附件使用只写 R8。`engine.window_render` 验证支持对应采样数的设备实际提交了反射深度，水面仍反射镜像场景。macOS Metal Debug／Release 构建及 Release 渲染与性能分析用例通过。在同条件对照中，移动反射片元中位数从 6.19 ms 降至 3.94 ms，整帧 P50／P95 从 44.20／44.80 ms 变为 43.72／44.71 ms，GPU 占用中位数减少约 53.8 MiB。静止偏移整帧 P50 从 27.67 ms 降至 26.14 ms，两组前后截图的 RGBA 像素相同。

方向光和局部灯按连续区间求值，VSM 页表直接编码物理采样坐标。`engine.window_render` 比对正常簇与溢出时的累加像素，`engine.render_foundation` 校验底左原点采样坐标，`engine.render_submission` 覆盖实际阴影和反射。内置和受影响的 Showcase Shader 完成 Metal、SPIR-V 与 WGSL 编译。相同全开移动轨迹的主视图不透明片元中位数从 15.87 降至 15.22 ms，整帧 P50／P95 从 43.72／44.71 降至 42.86／43.58 ms。静止偏移 P50 从 26.14 降至 25.35 ms，P95 从 27.44 变为 27.98 ms，两组截图的 RGBA 像素相同。

合并单采样深度与 SSR 材质表面提交通过 macOS Debug／Release 构建与 Release 渲染、性能分析用例。`engine.render_submission` 校验通道提交、裁切和共面遮挡及 SSR 开关切换。相同全开移动轨迹中，这两个通道的每帧绘制由 1874 次减至 937 次，整帧 P50／P95 从 42.86／43.58 ms 降至 42.05／42.84 ms。静止偏移 P50／P95 从 25.35／27.98 ms 变为 25.48／29.64 ms。两组截图的 RGBA 像素相同。

VSM 页深度范围计算及不透明 Shader 完成 Metal、SPIR-V 和 WGSL 编译。Release `engine.render_submission` 覆盖页缓存、移动后反馈、投射物变更与重建的像素对照。相同原生全屏全开采样中，移动主视图不透明片元中位数从 15.22 ms 降至 13.95 ms，整帧 P50／P95 从 42.05／42.84 ms 降至 41.01／41.87 ms。静止偏移 P50／P95 从 25.48／29.64 ms 降至 24.42／26.97 ms。两张对照图逐像素一致，VSM 纹理占用增加 2.33 MiB。两组计时区间的 VSM 页和深度范围均已预热。

不透明 PBR 的普通 UV 版本通过 macOS Debug／Release 构建和 Metal、SPIR-V、WGSL Shader 编译。Release `engine.render_submission` 和 `engine.window_render` 分别验证材质工作流切换、六边形去平铺开启和恢复后的画面。相同原生全屏全开采样中，移动整帧 P50／P95 从 41.01／41.87 ms 降至 39.64／40.39 ms，不透明片元中位数从 13.95 降至 12.92 ms。静止偏移 P50／P95 从 24.42／26.97 ms 降至 23.09／24.92 ms。两张对照图逐像素一致，预热后的静止、移动组每帧使用新程序的绘制中位数分别为 923、938。

2026-11-08，Windows x64 / Release / DirectX 12 / QuickJS，RTX 4070 Laptop，驱动 `31.0.15.4601`；1280×720、MSAA 1、VSync 关闭、Tracy OFF。优化前源码为 `5070daf9`，实现和当时完整夹具见 `ec7d74d7`。

主场景为 4,096 个共享网格/材质的静态对象；120 稳定帧后采集 600 帧，交替执行三对。下表为 render 调用的 P50 / P95 / P99（ms），包含提交与等待，不是纯 CPU 耗时。

| 组 | 优化前 | 优化后 |
| --- | --- | --- |
| 1 | 199.535 / 211.465 / 227.562 | 19.921 / 21.162 / 23.710 |
| 2 | 198.367 / 210.198 / 223.172 | 20.139 / 22.801 / 24.569 |
| 3 | 193.828 / 205.472 / 213.961 | 19.885 / 21.668 / 24.924 |

主视图始终为 4,096 可见对象、4,096 实例、524,288 三角形，全设备 draw 为 9。GPU P50 为 2.4–3.2 ms。相对改善约 90%，但 render 约 20 ms 仍超过 60 FPS 的整帧预算；CPU 分段诊断见下节。

当时的辅助场景各执行一对，结果只作方向与正确性检查：

| 场景 | 对象 | render P50 前 → 后（ms） | 全设备 draw 前 → 后 |
| --- | ---: | --- | --- |
| probes | 4,096 | 472.935 → 21.443 | 12 → 12 |
| mixed | 1,024 | 86.025 → 5.825 | 214 → 214 |
| dynamic | 1,024 | 88.363 → 5.783 | 214 → 214 |
| shadow | 4,096 | 240.019 → 23.549 | 20,595 → 28 |
| views | 1,024 | 105.211 → 6.184 | 295 → 295 |

Probe 使用 120 + 600 帧，其余辅助场景使用 30 + 120 帧。全部 8 对 HDR 图像完全相同；逐帧主视图计数相同，7 对具有逐 view 数据的结果也相同。mixed 优化后最大帧 37.414 ms，未剔除慢帧。首次同步准备从 20.691–26.146 ms 增至 61.590–67.919 ms；阴影瞬态缓冲从 1.3125 MiB 增至 2.5684 MiB。稳态 GPU 资源存量未增加；未采集 CPU 缓存容量/RSS，也未测完整冷启动，不宣称整体内存或启动收益。

原始二进制、指纹、逐帧 CSV、图像和核对结果保留在忽略的 `build/performance/render-submission/`；基线失败结果不计入有效样本。完整历史数据可从上述提交与本地原始结果查阅，不在当前文档重复逐次日志。

当前基准只保留上述 shared 负载，不再维护临时六场景矩阵。默认 4,096 对象、30 稳定帧、120 计时帧；仅在性能任务中显式运行，输出目录必须为新目录：

```powershell
.\build\windows-static-release\tests\engine_render_submission_test.exe --benchmark --objects 4096 --settle-frames 30 --frames 120 --output build/performance/render-submission/sample
```

比较时两版使用相同参数，不能把新短窗口直接与历史长窗口拼成严格对比。图像回读和 CSV 写盘在计时窗口外；计时期间不并行构建或跑其他测试。细分 CPU/GPU 原因使用[既有性能工具](runtime/performance.md)，不另写一套采样框架。

### CPU 分段诊断

基于 `cbb190b8` 加入内部 Tracy 分段，沿用上述 Windows/DX12 设备与 shared 场景。30 稳定帧后采集 120 帧，各运行一次未连接对照和 Tracy 0.14.1 on-demand 采集；输出及源码差异、二进制指纹位于忽略目录 `build/performance/render-cpu-profile/`。

未连接时 render P50 为 19.972 ms，连接时为 20.908 ms，相差约 0.94 ms（4.7%）；这是采集扰动检查，不是优化收益。开始/结束消息限定的窗口包含完整 120 次 Render.frame，以及 119 个完整 FrameMark 间隔。下表使用同一窗口内各区间总耗时除以 120，列出每帧平均值；子区间不再加到父区间上。

| 区间 | 每帧平均（ms） | 含义 |
| --- | ---: | --- |
| `Render.frame` | 21.183 | render 调用总计 |
| `Render.collectFrame` | 16.497 | ECS 对象与帧数据收集，约占总计 78% |
| `Render.meshState` | 6.537 | 上项内部：网格、形变状态、世界/法线矩阵 |
| `Render.meshFlagsAndLighting` | 7.514 | 上项内部：组件标志、实例化资格、光图与 Probe 查询 |
| `Render.meshMaterials` | 0.189 | 上项内部：材质绑定归组 |
| `Render.collectView` | 2.275 | 视图可见性、LOD 与绘制项收集 |
| `Render.buildBatches` | 1.532 | 各绘制通道的批次组织合计 |
| `Render.instanceData` | 0.122 | 实例缓冲分配和数据填充合计 |
| `Render.shadowSubmit` | 0.587 | 阴影提交路径 |
| `Render.frameSubmit` | 0.022 | bgfx::frame 调用，含可能等待 |

总 draw 仍为 9，连接采样的 GPU P50 为 1.509 ms。bgfx 的 `waitSubmit` 平均 19.643 ms 是后端等待提交线程推进；`waitRender` 平均 0.001 ms 是提交线程等待后端完成，两者不能颠倒。GPU 与 CPU 使用异步时钟，不直接相减。

修正前的热点集中在帧/视图数据收集，两项约占 89%。当时 `collect_frame` 已查询 ECS 列，仍通过 Node/Component 句柄逐项读取并重复校验。此诊断没有拆出各 getter、资源查找及矩阵计算的独立占比，不能将 16.497 ms 全部归于某一个函数。

上述连接运行在写出结果后退出超时，所属进程在 90 秒超时后结束；退出阻塞尚未定位。采集已包含完整计时窗口及结束标记，可用于上述 CPU 诊断，不作为正常退出或生命周期验收。未连接对照正常退出。

当前实现改为在稳定阶段直接读取网格 Data、ComponentState、NodeRuntime、WorldTransform 与姿态 Runtime；每个视图只借用一次相机隐藏名单，无 Lightmap 引用时跳过绑定检查。资源准备身份、版本校验和已有烘焙有效性规则保留，公开句柄契约不变，详见[收集与失效规则](runtime/class-diagrams/render-submission.md#收集更新与失效)。

该修改在同一设备、分辨率与 shared 场景执行一对 Tracy OFF 对比，均为 30 稳定帧 + 120 计时帧；不与上方连接采集或历史长窗口直接拼接：

| render 调用耗时（ms） | 修改前 | 修改后 |
| --- | ---: | ---: |
| P50 | 20.443 | 3.799 |
| P95 | 21.722 | 4.087 |
| P99 | 22.339 | 4.201 |

P50 减少约 81.4%。两次均正常退出，HDR 输出逐字节相同，全部逐帧/逐视图提交计数、GPU 资源数量与已记录字节数相同：4,096 可见对象及实例、524,288 三角形、全设备 9 draw。源码差异、二进制指纹及原始输出位于忽略目录 `build/performance/render-ecs/`。受影响目标 Debug/Release 构建通过，仅执行现有 Release `engine.render_submission` 一次并通过；没有新增测试程序或扩展性能场景。未重新采集优化后的 Tracy 分段，3.799 ms 是受控场景的 render 调用耗时，不代表游戏整帧或全场景性能。

## 验证范围与限制

- Windows x64 宿主已完成 `windows-static-release` 默认目标构建，覆盖运行宿主、编辑器、资源工具与 MCP 程序。`android-arm64-release` 已完成原生库、资源与 Java 编译并生成 Release APK，包内原生库仅有 `lib/arm64-v8a/libmain.so`。`web-wasm32-release` 已完成运行时编译、链接并生成 `engine.mjs`、`engine.wasm` 及成功链接回执。三端使用默认 QuickJS，受影响的 MCP 目标另通过 `windows-static-debug` 编译。此项验证范围为构建与 Android 打包，APK 沿用仓库默认测试签名。
- 源码英文字符串与外部 Unicode 夹具已完成 macOS arm64 的受影响 Debug / Release 构建；Debug 未运行测试。Release 的 GUID、日志、存储、资源读取、UI、脚本、MCP、资源管线、场景跨语言及建模几何共 18 项 CTest 通过；环境准备、Android 资源打包和桌面导出的 55 项脚本检查通过，私有项目 TypeScript 与设置检查通过。二十面球体（`icosphere`）曾因极点 UV 合并相反切线而失败；当前已修复并通过 `modeling.core`，新增回归覆盖多个细分级别、接缝和极点的单位正交切线。
- Schema 分层已完成 Windows Debug / Release 构建，Release 的 `modeling.pipeline`、`editor.destruction_pipeline` 和 `engine.scene_schema_cross_language` 通过，编辑器 TypeScript 通过严格类型检查。架构检查覆盖生成数据目标的所属层级与依赖方向。
- 建模参数、物理尺寸 UV、边界查询及可撤销重建已完成 Windows Debug / Release 构建；Release 的 `modeling.core`、`modeling.geometry`、`modeling.commands`、`modeling.pipeline`、`editor.scene_persistence` 和 `engine.mcp_registry` 通过。复用现有用例，未运行 Debug 测试、实窗或性能采样；不据此认定画面质量与其他平台已验收。示例源数据与资源包状态见 [Showcase](../projects/showcase/docs/index.md#验证)。
- 渲染提交优化已完成受影响 Debug / Release 构建，Release 的 `engine.render_foundation`、`engine.scene_hdr_capture`、`engine.window_render`、`engine.render_submission` 通过；这些结果只代表当时 Windows / DirectX 12 配置。
- 当前本机完整 Release 测试结果见上面的[场景加载与运行态测试](#场景加载与运行态测试)，不代表全平台通过。其他平台的旧执行日志保留在 Git 和本地 `build/`，不能直接当作当前版本的验收。
- iOS 完整宿主与交付尚未完成；Android 模拟器、独立依赖探针和桌面源码验证不代表实体手机的功能或性能。
- 该性能基线未覆盖 Shader/PSO 预热、大场景完整首帧及跨平台性能；CPU 分段结果仅适用于上述受控静态负载。

结果只记录必要信息：修改、配置、实际执行的目标/用例、结果、尚未验证的边界。更新本节或所属模块的现状，不再按每次会话追加一章。

## WebGPU 导出

构建入口见[构建指南](building.md#webgpu-游戏导出)。验收复用公开 showcase 的运行包，正式产物保持 `ENGINE_ENABLE_TEST_SUPPORT=OFF`，不依赖运行态 MCP 或临时脚本接口。平台测试使用相同入口、RuntimeHost、资源目录和普通运行时截图接口。

- `platform.web_bootstrap` 与 `platform.web_storage` 使用 Node.js 验证首帧就绪、取消、重复停止、故障终态、存储提交和失败回滚；入口分别为 `tests/runtime/app/web_bootstrap_test.mjs` 与 `tests/runtime/storage/web_storage_test.mjs`。
- `engine.desktop_export` 同时覆盖 Web 的链接回执、独立资源、配置、错误产物拒绝及失败保留旧输出。`engine_asset_build_test --web` 使用真实宿主 shaderc 验证所有内置 Shader 的普通、实例化、surface 和水面程序均匹配 Web 目标；`ENGINE_ENABLE_WEB_EXPORT_TOOLS=ON` 时登记为 `editor.asset_build_web`。
- `engine_web_render_test` 只在 Web 的 `ENGINE_BUILD_TESTS=ON` 时创建，复用正式 Web 入口和系统装配，只增加世界拥有的测试子系统。它通过普通 `request_screenshot` 等待实际 PNG，调整窗口后再次回读，发布浏览器测试回执；正式 `engine_project_app` 不包含该源码、声明和回执。

```sh
node --experimental-vm-modules --test tests/runtime/app/web_bootstrap_test.mjs tests/runtime/storage/web_storage_test.mjs
cmake --preset web-wasm32-release -DENGINE_BUILD_TESTS=ON
cmake --build --preset web-wasm32-release --target engine_web_render_test
```

实窗回读验证使用已导出 showcase 的缓存副本，将上述目标 `build/web-wasm32-release/web-render-test/` 中的两个运行文件复制进去，移除副本的正式 `web-build.json`，通过附带 HTTP 服务打开该副本。启动页面后检查 `globalThis.webRenderTest` 回执与其中的 PNG blob；验证后恢复 `ENGINE_BUILD_TESTS=OFF` 并使用正式导出目标。

持久化闭环使用 [Web 存储夹具](../tests/runtime/storage/fixtures/web-game.ts)：在缓存中复制公开项目，以该文件替换副本的 `assets/scripts/game.ts`，保留邻接 meta，使用独立产品身份导出。夹具通过普通 TS `Storage.Save()` 写入递增值并记录回执；成功后直接刷新验证旧值，浏览器对该测试应用的 KV 写事务注入一次 abort，再确认失败及正常停止均保留旧值。正常路径还在 `OnShutdown` 中 Save，验证代理队列关闭顺序。测试夹具和运行副本不得用于正式发布。

已在 macOS arm64、Chrome 156 的真实 WebGPU 上验证公开 PBR 和室内场景、菜单往返、正常停止／重启、普通运行时 PNG 回读与 2400×1350 到 1280×720 的缩放，以及 C++ Save 到 IndexedDB 的成功、刷新、中止回滚与关闭保存。截图与回执保存在本地忽略的 `build/research/webgpu/`。其他浏览器和设备、全部 showcase 功能以及真实配额耗尽尚未逐项验收；不据此宣称完整跨浏览器或所有可选效果等价。首版要求完整预载和 COOP/COEP，不支持编辑器 Web 化、后台流式资源下载或 V8。


## iOS 原生运行包

iOS 使用公开 showcase 与现有模块用例，运行时类及生命周期见 [iOS 宿主](runtime/class-diagrams/ios-host.md)，构建和签名入口见 [构建指南](building.md#ios-原生游戏导出)。

已在 macOS arm64 / Xcode iPhoneOS 与 iPhoneSimulator 27.2 SDK 上完成真机和 arm64 模拟器的 Debug、Release 编译，所选资源、QuickJS、角色、镜头、重定向、导航、特效、地形和破坏插件参与实际链接；最低部署设置为 17.0。Release 普通包和显式 MCP 测试包均可导出，公开范例资源快照为 1370 项。模拟器包的 ad-hoc 签名已验证；真机包目前为 unsigned，未使用开发者证书安装。

- `engine.runtime_host` 覆盖显式暂停、不提交帧、影片时间冻结、恢复首帧零增量、拒绝 worker 调用，以及事件回调只锁存故障并在安全点清理场景。
- `engine.script_runtime` 覆盖 Window.GetSafeArea 的活动上下文限制和逻辑坐标范围。
- `engine.font_ui_render` 覆盖渲染资源释放后保留原文档、控件句柄、动态文本和样式，重新绘制并继续接收原控件事件。
- `engine.desktop_export` 中的 iOS 用例覆盖独立资源布局、SDK / 测试包身份拒绝、陈旧二进制拒绝、签名失败保留旧包、独立 App ID Prefix、受限通配授权、默认 keychain group、签名后权限回读及证书授权，以及 arm64 真机对象不能冒充模拟器对象。
- 正式链接回执与符号检查用于确认运行端点和临时脚本只存在于测试构建；iOS MCP 复用现有文件通道，不另建协议或加载状态。

arm64 iOS 26.2 模拟器已安装并启动普通包及测试包，实际后端为 Metal，画面为 1206×2622、逻辑窗口为 402×874。公开菜单、PBR 方块和公寓场景均已显示；测试端点在当前 Gameplay 中执行临时脚本、查询安全矩形，并按场景身份和设备代次完成 PNG 截图回读。该结果只覆盖这两个 3D 范例，不代表全部菜单场景已在 iOS 逐项验收。

前后台切换时设备进入不可绘制状态；恢复后设备代次递增，原会话、Run、场景、文档及控件句柄保留，公寓和菜单均可继续绘制并截图。设备释放调用 UiSystem.release_gpu，保留 CPU 文档、运行时修改和订阅；停止会话仍完整释放文档。普通 App.RequestQuit 经帧安全点关闭并发布 closed 状态；不以系统强制终止替代正常退出验收。同一持久目录下 Storage.Save 后终止并重新启动测试应用，已确认键值保留。测试通道参数会显式覆盖持久目录，不能把更换通道目录当作同目录存档重启测试。

Android、iOS 使用同一安全区域查询；本次实际坐标仅在 iOS 模拟器验收，Android 设备未补测。核心仍全屏绘制，菜单边距由公开范例脚本处理。真实触摸、旋转、音频中断及真机后台 GPU 行为尚未验收；真机包未签名。SDL/UIKit 在启动和前后台切换时仍输出 appearance-transition 警告，当前未造成退出或画面丢失，保留为设备验收项。模拟器通过不能代表真机通过。

资源同步复用 `engine.asset_sync`；`engine.android_mcp` 与 `engine.ios_mcp` 分别回归设备目标、完整目录、损坏资源拒绝、锁、原子控制及状态真实性。iOS 容器用例覆盖首次启动前只读查询、未上报／最近停止、首次控制写入建目录、多块载体、符号链接拒绝、异平台 catalog 拒绝、旧版本保留及同版本不重复发布。`engine.runtime_host` 额外验证准备身份与 catalog 内容版本不同的情况。模拟器在不装配运行态 MCP 的普通 Release 应用中，同步完整 1,370 项资源后启动，再同步改变的 UI 和脚本，已确认画面更新、Run 切换，以及 activeRevision 与 availableRevision 一致。真机 House Arrest/AFC 传输已参与 Debug / Release 编译链接，尚未用已签名真机验收。

本地构建日志、导出测试结果与回执位于忽略的 `build/research/ios-runtime/`，产物位于 `output/ios/arm64/`。平台补丁及用途见 [iOS 依赖说明](../third_party/notices/ios.md)。
