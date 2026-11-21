# 性能采样

本页只维护可复用的工具、口径和已知边界。性能采样由明确的性能任务触发；默认一个场景、一对前后结果，出现噪声或结论不足时才增加轮次。当前渲染数据见[性能基线](../testing.md#渲染性能基线)，不在多份文档重复维护。

## 开关与职责

- `ENGINE_ENABLE_TRACY` 默认 OFF；开启时目标私有链接同版本 Tracy on-demand 客户端，公开核心接口不依赖 Tracy。
- Tracy 构建可追加 `-DENGINE_ENABLE_TRACY_FUNCTIONS=ON`，记录实现函数、头文件内联、模板和闭包的运行期调用区间。函数标记适合短窗口追调用路径，常规采样默认只启用命名阶段和统计指标。
- 头文件和 Engine／System 基础框架通过标准库探针把函数区间交给宿主。核心库将同一编译开关传给消费者，宿主装配 Tracy 采集端。常量求值保持原行为，区间由 Tracy 自身处理连接切换和结束配对。
- Web 页面加上 `engine-profile=1` 查询参数后，浏览器性能录制可看到 `Engine.web.*` 的启动、资源目录和存储区间。区间提交后会从页面的计时缓冲清除。
- Android 的 Java Activity 和 Surface 使用系统追踪区间 `Engine.Android.*`，可在该应用的系统性能记录中查看生命周期和渲染交接。
- 命名阶段使用 `src/engine/runtime/performance/profiling.hpp`。头文件和基础框架使用 `runtime/performance/function_profile.hpp`。关闭时不查找、编译或链接 Tracy。
- 计时底层、故障处理和采集器自身使用各自的外层调用边界。分配器回调通过命名池的内存事件记录，避免在观测路径内再次进入同一采集接口。
- 主 FrameMark 位于编辑器／运行宿主的帧循环，包含系统、玩法、渲染、编辑工具和帧末等待；`Render` 是独立的命名帧序列。`Host.frame`、`Render.frame` 是 CPU 区间，后者包含提交与等待，不能当作完整宿主帧。
- Tracy 依赖特性同时启用 bgfx 的 `profiler` 特性。配置检查实际导入库是否公开 `BGFX_CONFIG_PROFILER=1`，不能只给引擎头文件补宏冒充后端已启用。现有 `RendererCallbacks` 将内部 begin/end 回调桥接到 Tracy，复制临时视图名称，并保持按线程配对；Engine／System 基础框架仍不依赖 Tracy。
- bgfx 状态面板由已有 MCP 开关控制，与 Tracy 构建开关独立，默认关闭。
- Tracy 构建只在采集器连接期间启用 bgfx 视图时间戳采样，不显示统计面板；连接状态在可绘制帧准备时更新，断开后停止阶段采样，避免日常运行持续承担原生通道拆分开销。`Bgfx.cpu/gpu*Ms` 按反射、阴影、深度、材质表面、不透明、AO、SSR、透明、后处理、呈现、Meshlet、RVT 和 VSM 反馈分组。GPU 分组图只描述对应已完成帧的有效记录；原始 `Render.cpuView`、`Render.gpuView` 和 `Render.gpuFrame` 消息分别保留计数器、频率和 GPU 源帧号，离线去重，不丢弃独立延迟返回的视图。

## 分段与内存

`ENGINE_TRACY_MEMORY_MODE` 默认为 `off`。`totals` 保留已覆盖分配池的当前容量、峰值和存活笔数，可在 Web 构建中独立于 Tracy 客户端使用。`detailed` 需要 `ENGINE_ENABLE_TRACY=ON`，连接时记录命名池逐笔分配与释放，支持调用栈的平台同时记录两侧的栈。

| 分配池 | 当前覆盖 | 统计粒度 |
| --- | --- | --- |
| `Engine.ResourceRead` | ResourceSystem 接收完整资源封包后的同步与异步读取缓冲 | `ByteBuffer` 实际容量，逐笔 |
| `Engine.ResourceDecode` | 资源封包验证后到单项解码结束之间的载荷副本 | `ByteBuffer` 实际容量，逐笔 |
| `Engine.TexturePrepare` | 纹理任务持有的转码像素，随任务销毁或替换归还 | 像素容器实际容量，逐笔 |
| `Engine.TextureUpload` | 交给 bgfx 的上传副本，释放回调归还 | CPU 副本字节，逐笔 |
| `Engine.QuickJS` | QuickJS 拥有的 arena 与大块，跟踪创建、重分配和释放 | 传给宿主分配器的请求字节，逐笔 |
| `Engine.FrameReadback` | 屏幕截图的 CPU 读回缓冲，随渲染器销毁释放 | 像素容器实际容量，逐笔 |
| `Engine.ScreenshotEncode` | 编码和写盘持有的像素副本，随编码任务归还 | 像素容器实际容量，逐笔 |
| `Engine.BgfxCpu` | bgfx 通过 `AllocatorI` 创建的 CPU 块，覆盖初始化、渲染线程及设备关闭 | 调用者请求的字节，逐笔 |
| `Engine.Jolt` | 由引擎安装的 Jolt 常规与对齐分配回调，覆盖物理工厂及其分配 | 调用者请求的字节，逐笔 |
| `Engine.Flecs` | 启动首个 WorldSystem 时还未配置的 Flecs OS 内存接口，覆盖 ECS 堆块、arena 与字符串 | 向接口请求的字节，逐笔 |

`memory_profile::snapshot()` 返回一次读取时的计数、单调时钟时间戳、数据变更序号和 Tracy 连接标识。曲线里的 `Memory.captureGaps` 累计连接前未采集的分配以及跨过断连的事件。离线期间容量仍有记账，连接前存活的对象可从当前量看到。MCP 和 Web 的池字段 `hasSamples` 表示实际记录过分配，值为 false 时尚无该池证据。读取端当前提供命名池总量。

QuickJS 的对象会在已取得的 arena 内复用空间，分配池只累计实际交给虚拟机的底层块。上传副本的释放由 bgfx 回调确认，GPU 完成和驱动回收由对应层提供。诊断构建的内存块包含少量追踪元数据，该开销由进程或 Wasm 总量观测。

编辑器或测试运行端的 MCP 命令 `get_resource_memory` 可用 `guid` 查询指定资源，或用 `limit` 查询最大项，默认 32 项，上限 64 项。排序使用已测的 CPU 主载荷容量，未测项使用包内载荷大小。结果的 `totals` 汇总整个资源图，每份存活记录只计一次。把上次的 `totals` 传为 `baseline`，可以比较同一加载会话的已测容量和覆盖数量。每个记录列出获取、依赖和请求，超出 32 条时保留完整计数并标明截断。CPU 命名池、连接与采集缺口在 `cpuProfiler` 中单独报告。记录阶段与持有关系见[资源内存快照](../plugins/runtime/class-diagrams/resource-graph.md#资源内存快照)。

MCP 和 Web 的 `gpuMemory` 使用同一个渲染快照。当前覆盖场景纹理、网格基础几何、共享顶点页、呈现目标与截图回读。Tracy 的 `Render.gpuLogicalLiveBytes`、`Render.gpuLogicalRetiringBytes` 和同组 `gpuSharedBacking` 曲线与快照使用相同口径。网格逻辑量含页内子范围，顶点页容量以独立记录计一次。驱动报告量在 `driverBytes` 独立提供，可用性随平台变化。这三组数值存在重叠。

`pending` 报告等待回读、渲染退役或后端销毁处理的记录，`recentReclaims` 从最近的 128 条回收记录中返回匹配项。各列表有总匹配数，`overwrittenHistory` 表示超出历史容量的记录数。纹理退役的各时点以及截图回读实际完成由 [RenderSystem](class-diagrams/render-system.md#渲染内存快照) 定义。常规纹理、网格的后端确认表示 bgfx 已处理销毁命令，驱动内的物理释放仍依赖平台工具。

Web 的 `totals` 构建从 `bootstrap.mjs` 提供只读异步函数 `memory_snapshot()`，返回当前场景、分配池计数、序号、时间戳、采集连接与缺口状态，以及 `linearBytes`、`allocatorLiveBytes` 和 `allocatorFreeBytes`。Wasm 线性容量包含分配器保留空间、栈及其他内存，分配池是存活分配的已覆盖部分。默认的 `off` 构建不导出原生查询入口。结合 `Engine.web.bootstrap.module`、`Engine.web.bootstrap.callMain` 和 `Engine.web.bootstrap.onEngineReady` 可以对齐模块初始化、宿主启动及首帧。浏览器进程 RSS 与 Wasm 容量需要分别从平台和引擎读取。

在 Web 预设上追加 `-DENGINE_TRACY_MEMORY_MODE=totals -DENGINE_WEB_HEAP_OBSERVATION=ON`，可在同一查询中查看 `generalHeap`。它从宿主进入 `main` 开始记录链接到 Wasm C 分配入口的请求，底层仍使用 SDK 的 dlmalloc。它提供当前量、峰值、最大 16 笔未归属分配、最近 16 笔释放以及分配身份与释放序号。记录没有浏览器或原生调用栈，启动前的分配、SDK 内部直连分配器、浏览器、驱动和 JavaScript 堆需要各自的分析入口。

`unattributed` 可与现有命名池一起分析。已归属的普通堆块列在 `attributed`，其请求大小与命名池中的容量可能略有差别，汇总时采用命名池的一侧。由命名池直接申请的底层缓冲在申请时只进入命名池。`droppedAllocations` 报告超过记录容量的分配，`overwrittenReleases` 表示已经移出最近释放窗口的记录。`unmatchedReleases` 和 `unmatchedAttributions` 报告缺少对应通用堆记录的操作，`ownershipConflicts` 报告可观测记录与归属冲突。`recordCapacity` 和 `metadataBytes` 分别报告上限和跟踪表容量。诊断开关默认关闭。

macOS 诊断预设可启用 `-DENGINE_MACOS_HEAP_OBSERVATION=ON`，配合 `totals` 或 `detailed` 观测经过系统分配入口的普通 C/C++ 堆请求。覆盖标准分配与对齐接口、Darwin 类型分配入口及常用 malloc zone 接口。编辑器和测试运行端的 `cpuProfiler.generalHeap` 提供与 Web 相同的容量、归属及历史字段。经专用堆或内部分配器私有入口的内存仍由对应命名池、平台分析器或模块统计提供。

详细模式在 Tracy 的 `Engine.GeneralHeap` 池提供分配与释放调用栈。底层块进入命名池时结束通用堆事件，解除命名归属时续记其余生命周期。`Memory.generalHeapBytes` 绘制通用堆的未归属请求容量，`captureGaps` 标出因未连接或断连错开的事件。诊断宿主通过独立的动态库接入系统分配入口，macOS 导出时随宿主收集。

在详细或总量的 Tracy 构建中，可用 `ENGINE_TRACY_WAIT_FOR_CAPTURE_MS` 将启动追踪连接等待限制在 0 至 30000 毫秒。运行宿主在开始初始化呈现系统前等待，超时按原流程启动。采集器先记录连接，接着加载的资源会出现在同一条时间线上。正式默认构建保持 `ENGINE_TRACY_MEMORY_MODE=off`。

macOS 若客户端实时符号解析返回 `[error]`，启动时设置 `TRACY_SYMBOL_OFFLINE_RESOLVE=1`，让 Tracy 保存镜像和相对地址。保留匹配的 Release 可执行文件，使用 `xcrun atos -o <executable> -offset <offset>` 解析调用栈。

宿主标记分别覆盖事件、任务完成派发、系统更新、音频、玩法、影片、场景准备；编辑器工具和资源服务另列。场景资源准备拆分脏网格、资源集合、程序、贴图任务、材质绑定、Meshlet、通道目标及进度统计，绘制按收集、光照／阴影、深度、表面、AO、不透明、SSR、透明、扩展与呈现划分。`Render.outputPresentSubmit` 与 `Render.bgfxFrame` 分开，避免将后端换帧等待全部归为呈现绘制。

bgfx 内部标记覆盖线程交接、排序及后端提交；Metal 补丁另标记获取 drawable、帧延迟等待、命令提交／完成、GPU 时间戳解析和渲染 PSO 创建。其名称保留在原始 trace，中文报告提供对应说明。窗口未经过的阶段不宣称已验证；启动和 PSO 首次创建需要另取对应窗口。

Metal 阶段计数器只接受当前校准采样区间内、完整且有序的起止样本。复用采样缓冲时，仅清理附件或未执行的阶段可能遗留旧值；旧值、单端点和错误值不能拼成有效的 GPU 时长。缺失的视图标为未采集，不用零或整帧时间补齐；完整命令缓冲的独立时间记录继续保留。

支持阶段边界采样的 Metal 设备上，采集期间每个计算视图使用独立编码器，记录实际计算编码器的起止时间。绘制与计算编码器沿用 bgfx 视图名，便于和原生 Metal 记录对应。常规运行继续沿用原有编码流程；性能验收配对采集未连接分析器的宿主帧间隔。

离线分析按 GPU 源帧统计阶段内视图区间的并集，报告有效帧数、P50、P95、全视图并集和重复计入量。跨阶段区间仍可能交叠。纯拷贝记录 CPU 编码和提交次数，合成中的多采样颜色初始化另有绘制视图。

连接采集器时，渲染报告增加以下观测；未连接不遍历资源计算这些统计，关闭 Tracy 时不编译该路径：

| 指标 | 口径 |
| --- | --- |
| `Render.meshCpuArrayBytes` / `Render.meshletCpuArrayBytes` | 渲染持有的唯一源网格数组容量，基础数据与 Meshlet 专用数组分列 |
| `Render.textureCpuArrayBytes` | 唯一贴图资源的像素与子资源数组容量 |
| `Render.sharedGeometryGpuEstimatedBytes` | 已创建共享静态几何缓冲估算，不含实例、蒙皮和 Meshlet 工作缓冲 |
| `Render.textureStagingBytes` / `Render.preparePending*` | 准备暂存字节与待完成纹理、材质、程序数 |
| `Meshlet.candidatePairs` / `indirectCommandSlots` / `computeBatches` / `budgetFallbacks` | 实际候选、提交命令槽、计算批次和预算回退；槽数不等于可见命令数 |
| `Meshlet.workspaceGpuBytes` | 输入、实例、描述数组和间接命令的当前申请容量，不含共享几何、驱动对齐或驻留；不能替代 RSS |
| `Vsm.*Pages` / `Rvt.*Pages` / `*.residentBytes` | 页面需求、就绪或重绘状态和所属 GPU 缓存容量。VSM 另记录缺页回退、反馈复用和读回环满时的跳过 |
| `ResourceGraph.records` / `acquisitions` / `requests` | 资源图持有规模与请求数量 |

`Render.profileMemory` 和 `Render.profileStats` 单独记录统计本身的开销。CPU 数组统计不覆盖全部分配，GPU 缓冲估算也与驱动内存口径不同。Metal 在 `Bgfx.gpuResidentBytes` 中报告 `currentAllocatedSize`，即设备分配量。各项不能与 RSS 加减求“剩余内存”。CPU future 完成、CPU 帧末与 GPU 完成使用不同时间点。

## 固定操作步骤

1. 固定模块、场景、对象数量、资源版本、镜头、操作序列、分辨率、效果、VSync 与帧率限制；使用同一实际设备和 Release 配置。
2. 保存代码版本、二进制指纹、编译开关、设备/驱动和采样参数。初始化与资源准备单列，不混入稳态渲染。
3. 一个 trace 只采一轮明确负载。计时期间不并行构建、导入、截图、测试或离线分析；慢帧保留，不事后删异常值制造收益。
4. 对同一窗口查看 self / inclusive、主线程工作与等待、GPU、实际工作量和内存。优先处理当前模块的热点，其他模块只记录。
5. 固定负载比较一对结果；只在波动、失败或未解释退化时追加。没有分阶段采样时，只报告整体耗时，不能宣称具体函数的加速比例。
6. 原始产物放独立记录根目录的 `records/performance/raw/<run>/`，失败结果不覆盖；每轮保留截图、文档快照、源码补丁、参数、版本指纹和完整采样资料。记录根目录由 `ENGINE_RECORDS_ROOT` 或忽略的 `.cache/record-storage.json` 显式配置；每轮开始先检查外置卷；不可用时使用显式配置的 `fallbackRoot` 内置备用目录，两处使用相同分类且不写回旧记录目录。结束时正常停止本次项目和进程；当前摘要更新到[测试文档](../testing.md)，不追加会话流水账。

## 工具使用

仓库工具位于 [scripts/performance](../../scripts/performance/)。分析器接收已有 trace，不启动引擎；采集包装器只创建官方 capture 子进程，可选只读同机 RSS，不终止目标进程。

引擎通过根 CMake Presets 构建，在需要采样的现有 Release 配置追加 `-DENGINE_ENABLE_TRACY=ON`。例如已准备依赖的 macOS 原生配置：

```sh
cmake --preset macos-native-release -DENGINE_ENABLE_TRACY=ON
cmake --build --preset macos-native-release --target editor_app engine_app mcp_app
```

其他平台使用对应根预设；环境与依赖准备见[构建指南](../building.md)。采样结束后的正式构建重新配置为 `-DENGINE_ENABLE_TRACY=OFF` 并增量构建，不为切换开关删除依赖缓存。

工具客户端与服务器使用同一 Tracy 版本。当前为 0.14.1，复用 vcpkg 已准备的上游源码。独立工具入口只构建官方 capture/csvexport 及一个使用官方 Worker API 导出 FrameMark 的小程序，不包含引擎、V8 或分析器 GUI。配置时提供本机已有的工具依赖前缀；工具依赖缺失时按 `scripts/performance/vcpkg.json` 的独立开发工具清单准备，不清理或重建整套引擎。使用两个工具构建目录，保留官方 capture 的 `NO_STATISTICS`，避免采集期间额外计算离线统计。宿主仍要求 Ninja 和 ccache（Windows 为 sccache）。

```sh
cmake -S scripts/performance -B build/performance-tools/capture -G Ninja \
  -DPERFORMANCE_TOOL=capture \
  -DTRACY_SOURCE_DIR="<tracy-0.14.1-source>" \
  -DCMAKE_PREFIX_PATH="<host-tool-dependencies-prefix>" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build/performance-tools/capture --target tracy-capture
cmake -S scripts/performance -B build/performance-tools/analyze -G Ninja \
  -DPERFORMANCE_TOOL=analyze \
  -DTRACY_SOURCE_DIR="<tracy-0.14.1-source>" \
  -DCMAKE_PREFIX_PATH="<host-tool-dependencies-prefix>" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build/performance-tools/analyze --target tracy-csvexport tracy-frameexport
```

采集 metadata 至少记录提交、二进制 SHA256、设备/驱动、后端、Release/Tracy 开关、项目/场景、资源版本及操作参数；PID 和本机绝对路径只保留在本地产物。

命令中的 `ENGINE_RECORDS_ROOT` 需先设为已有的独立记录根目录。本机配置文件使用 `root`、`fallbackRoot` 和可选的 `requiredMount` 字段；根目录以 `.engine-records-root` 文件标识，供记录入口检查；暂存目录为该根目录的 `scratch/`。每轮固定选定根目录，下一轮重新检查外置卷。配置不会修改资源导入缓存或编译缓存的位置。

启动编辑器或其他独立记录程序时，可使用 `python3 scripts/record_storage.py -- <program> <arguments>`，先选择外置或内置备用目录，再把同一根目录传给子进程。直接启动编辑器时，需显式设置相同的 `ENGINE_CAPTURE_ROOT` 与 `ENGINE_CAPTURE_FALLBACK_ROOT`；已启动的宿主在该轮内保持其截图根目录。项目自带的渲染运行入口自动执行这一步。

macOS/Linux 采集示例，`--pid` 为已核对的同机引擎 PID，不能用远端设备 PID：

```sh
python3 scripts/performance/capture.py \
  --capture build/performance-tools/capture/capture/tracy-capture \
  --output "$ENGINE_RECORDS_ROOT/records/performance/raw/before-6" \
  --metadata "$ENGINE_RECORDS_ROOT/records/performance/raw/metadata.json" \
  --pid <engine-pid> --seconds 35
```

包装器通过 PTY 确认官方连接消息，避免 stdout 缓冲造成“未连接”的误判；连接超时不执行任何场景操作。RSS 默认约 0.5 秒采一次，并校验 PID 的启动身份。远端客户端可指定 `--address`、`--port`，省略 `--pid`；设备内存另外采集。Windows 可直接用同版本官方 capture/GUI 保存 trace，再使用相同离线流程，当前包装器不宣称支持 Windows 进程采样。

```sh
python3 scripts/performance/summarize_trace.py \
  --trace "$ENGINE_RECORDS_ROOT/records/performance/raw/before-6/capture.tracy" \
  --csvexport build/performance-tools/analyze/csvexport/tracy-csvexport \
  --frameexport build/performance-tools/analyze/tracy-frameexport \
  --output-dir "$ENGINE_RECORDS_ROOT/records/performance/raw/before-6/analysis" \
  --message-prefix "Rocket range sample " --start-field fired --start-value 1 \
  --complete-field complete --frame-zone Host.frame
```

空闲场景省略消息参数，使用所有完整 FrameMark 间隔；其他业务可传自己的 JSON 消息前缀与完成字段，或者 `--start-ns`/`--end-ns`。一个 trace 只放一轮负载，避免把多次运行误合并。输出包含官方逐事件导出、帧间隔、`summary.json` 和自包含中文 `report.html`；原始输入与分析配置必须一同保留。`task_queue` 按域、任务和优先级汇总单行注释中的 `queued_ns`，分别输出排队与执行耗时、执行线程及缺失/无效注释数量；缺注释时不虚构排队数据。

中文报告由 `trace_report.py` 从摘要与邻接 `capture.json` 生成，语言文案独立放在 `locales/zh-cn.json`，原始标记、源码、线程和版本指纹保留。报告提供主线程耗时树、各线程自身热点、等待、逐 GPU 源帧的视图观测、任务排队及内存／工作量；父子和跨线程区间不重复相加。可合并同条件的一对摘要，无需重新采集或导出：

```sh
python3 scripts/performance/trace_report.py \
  --analysis "$ENGINE_RECORDS_ROOT/records/performance/raw/before-6/analysis/summary.json" \
  --analysis "$ENGINE_RECORDS_ROOT/records/performance/raw/after-6/analysis/summary.json" \
  --output "$ENGINE_RECORDS_ROOT/records/performance/reports/comparison.html"
```

多轮采样可以按输入顺序重复传入 `--label`，为每份摘要指定中文显示名，例如区分优化前、优化后或已撤回的试验；标签数量必须与 `--analysis` 一致。标签只影响报告，不改写原始 trace、计数或版本指纹。

macOS 深入定位时，用 Instruments 的 `Metal System Trace` 模板附加到同一个测试进程，并在 bgfx 视图采样开启的时段记录命名编码器。从该记录各导出一次 `metal-application-encoders-list` 和 `metal-gpu-intervals`。例如对两种 `schema` 值分别执行以下命令，使用同名 `.xml` 文件名保存。

```sh
xcrun xctrace export --input recording.trace \
  --xpath '/trace-toc/run[@number="1"]/data/table[@schema="metal-gpu-intervals"]' \
  --output metal-gpu-intervals.xml
```

对同一工作负载运行 `summarize_trace.py` 时，以 `--metal-tables <directory>` 传入两份 XML 的目录。报告通过编码器 ID 关联 GPU 活跃区间，按 GPU 源帧归并同名通道，也提供逐视图的顶点、片元和计算数据。没有命名或未导出的区间不参与统计。联合采样用于定位，帧率取未连接采集器的独立运行。

完整宿主帧、关键阶段、实际 bgfx 回调和原生视图记录应由具体测试流程按目标检查，不能仅以 trace 文件存在判定采集完成。分析器拒绝交叉的 CPU 区间、缺失的自身时间、倒序的原生时间戳及回调失配；失败保留原始输入。根帧的直接阶段覆盖率只说明顶层区间包住了多少时间，不表示所有函数已细分；线程覆盖与等待都是墙钟观测，不是 CPU 利用率。

## 统计口径

- 帧报告均值、P50/P95/P99/max、样本数及实际吞吐；说明分位数算法，保持两版一致。
- CPU 区间区分自身工作、子区间与等待，父子 inclusive 不相加；CPU/GPU 异步数据不能直接相减定位热点。
- RenderViewStats 是场景网格统计；阴影、后处理等总提交另取 bgfx Stats。合批可以减少 draw，不能减少应绘制实例或三角形。
- GPU 时间保留有效性与每条记录的 `gpuFrameNum`；没有可用时间时写“不可用”，不填零或以 CPU 墙钟替代。Metal 等后端的视图区间可能共享编码器、相互重叠，命令缓冲区间也可能跨流水帧重叠，不能相加成独占 Shader 工作时间或代替实际吞吐。
- 进程 RSS、CPU 暂存、纹理与 RenderTarget 估算分别报告，不能混称显存或内存收益；未测的项明确留空。
- 图像、功能与生命周期先满足契约；不能以降画质、少绘制、少算物理步或放宽容差换取性能结论。
- 场景和参数不同只能分别报告，不能算严格加速比例。短样本不代表持续热负载、完整首帧或移动设备性能。

地形专项沿用 `summarize_terrain_collision.py --analysis-dir <analysis> --output <new-file>`，不再另写分析器。同步提交、异步后台准备与主线程发布分开；缺少请求关联或完整埋点时报告未知，不按相邻时间猜测一次请求的总延迟。

## 并行观测

线程契约见[任务类图](class-diagrams/task-system.md)，项目额度见[并行预算](parallelism.md)。Task.execute 的注释包含域、任务、优先度和排队时间；Task.wait / shutdown 表示等待，不是任务 CPU 运算。

| 链路 | 需要区分的工作 |
| --- | --- |
| 文件与资源 | I/O、单项解码、依赖调度、结果发布、在途数量和字节 |
| 渲染 | 纹理准备/上传、帧收集、视图列表、组批、提交等待、GPU 与读回 |
| 物理与导航 | worker 工作、必要的本帧 barrier、主线程发布与同帧累计步数 |
| 动画 | 实例采样、join、结果发布；首次资源准备单列 |
| 地形与破坏 | 后台候选、主线程发布、同步连通扫描/塌方和物理更新 |
| 音频 | 参数发布、SDL 供数、初始化与设备生命周期；回调不能访问场景或 VM |

资源准备的项目数、时间和暂存预算不覆盖驱动队列、UI/字体或其他插件的全部开销。主线程仍处理平台事件、UI/VM、结果发布与必要的 join；并行不等于所有路径已经跨帧异步。

## 采样结果

Windows 渲染基线和代价见[测试记录](../testing.md#渲染性能基线)，Tracy 的收集、组批与等待占比见[CPU 分段诊断](../testing.md#cpu-分段诊断)。Shader/PSO 预热与整体启动速度尚未验证。

2026-11-01 的 macOS 单线程地形/物理样本、后续并行样本不再作为当前版本的性能依据；历史过程保留在 Git 与本地 `build/terrain-step5-jolt-boxes/`。当时 Jolt 单步与整体物理没有达到 0.5 ms，不能把局部碰撞更新的结果外推为整条链路达标，也不能将这些数字外推到 Windows 或手机。
