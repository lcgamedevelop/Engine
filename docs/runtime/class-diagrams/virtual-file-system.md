# 核心文件类

[类图索引](../../class-diagrams/index.md) · [统一生命周期](resource-lifecycle.md) · [资源读取](resource-system.md) · [目录](resource-catalog.md)

VirtualFileSystem 提供同步读取、独立异步操作及取消；文件层不包含资源计数、GUID、资源解析或依赖图。

下方普通文件契约服务原生调用方和 [File 脚本绑定](../../plugins/runtime/class-diagrams/file-script-api.md)；现有挂载读取与 FileRead 生命周期保持。

## 文件读取与任务

```mermaid
classDiagram
    class VirtualFileSystem {
        <<final>>
        +VirtualFileSystem()
        +~VirtualFileSystem()
        +configure(const MountList& mounts) bool
        +configure_async(TaskSystem& tasks, const FileAsyncConfig& config) bool
        +read(const FileLocationT& location, uint64_t offset, uint64_t size, ByteBuffer& output) bool
        +begin_read(const FileLocationT& location, uint64_t offset, uint64_t size, unique_ptr~FileRead~& output) bool
        +poll_async(uint32_t completion_budget) uint32_t
        +shutdown_async() void
        +mounts() const MountList&
        +error() FileError
        +last_error() string_view
        +read_file(const Path& path, ByteBuffer& output, string#42; error = nullptr) bool$
        +hash_file(const Path& path, uint64_t& hash, uint64_t& size) bool$
    }
    class ResourceMount {
        <<struct>>
        +string name
        +Path root
    }
    class FileAsyncConfig {
        <<struct>>
        +uint32_t max_in_flight = 4
        +uint64_t max_in_flight_bytes = 67108864
    }
    class TaskSystem
    class FileLocationT
    class FileRead
    class FileError
    class SDL3 {
        <<library>>
    }
    VirtualFileSystem *-- ResourceMount : 挂载值
    VirtualFileSystem *-- FileAsyncConfig
    VirtualFileSystem --> TaskSystem : 借用独立文件任务域
    VirtualFileSystem ..> FileLocationT : 范围定位输入
    VirtualFileSystem ..> FileRead : 转交独占操作
    VirtualFileSystem ..> FileError
    VirtualFileSystem ..> SDL3 : 私有文件及异步 I/O 后端
```

MountList 为 vector<ResourceMount>，Path 为 filesystem::path，ByteBuffer 为 vector<byte>，不是新框架类。FileLocationT 沿用 schema 生成模型。

- 普通类，不继承 System；实例由 ResourceSystem 独占并统一配置、轮询及关闭，普通文件静态方法不依赖该实例。构造/析构 noexcept，不允许复制；存在异步操作时不能移动对象。
- configure 校验并原子更新挂载，失败保留旧值，不扫描/打开文件。已受理操作持有自己的挂载快照，更新后不改变其来源；资源目录换代取消由上一层决定。
- read 的 offset 相对实体起点，size 是字节数；校验加法溢出、实体范围、实际文件边界和平台 size_t 容量。成功输出独占缓冲，失败清空；精确范围未读满视为失败。零长度合法范围成功返回空缓冲。
- read_file 是工具所需的整文件同步能力，不要求 ResourceSystem 初始化；可选 error 输出提供独立诊断，不修改实例错误状态。资源包成员统一用范围 read。hash_file 保持固定 64 KiB 缓冲的 FNV-1a 64 位整文件摘要，失败 hash/size 清零，不整包载入内存。
- begin_read 只受理请求，要求操作输出为空，非空则拒绝并保留原值；合法输入失败时输出空且不会产生晚到结果。未配置返回 unavailable，暂时满额返回 busy，单项超过字节上限返回 budget_exceeded。命令错误通过 error/last_error 获取。
- configure_async 借用已初始化 TaskSystem，在活动请求清空后才可重配。TaskSystem 负责打开和路径校验等短任务，实际传输使用 SDL 异步 I/O；不新建线程池，不让 CPU worker 主动等待传输。
- poll_async 非阻塞收集最多 completion_budget 项完成结果，仅由所属系统推进；业务查询 FileRead 不会推动工作。宿主统一派发 TaskSystem 完成委托。
- 普通取消不等待底层；若后端无法中断，则标记撤销并忽略晚到结果，I/O 完成前保留缓冲和句柄。只有 shutdown_async/析构等待收尾，关闭可重复；将仍未交付的 reading/ready 操作取消并回收结果，停止后不能发布新的完成结果。
- read/error/last_error 和异步提交在协调线程使用；mounts/error/last_error 为 const noexcept。read 更新最近命令错误，因此不声明 const；状态返回标注 nodiscard。
- SDL 类型、队列与平台路径实现私有；散文件和未压缩包内范围共用契约。Android 使用 APK assets 的真实 AAsset 来源，不能被同名私有文件遮挡；UTF-8 转换复用 PathUtils，保留越界、权限与符号链接检查。
- 文件层不选择持久版本、不回退旧包、不解码资源、不处理业务卸载。

## 普通文件与目录

普通文件与目录使用现有 VirtualFileSystem 的静态文件能力，不新增 FileSystem、文件 System 或资源服务包装层。read_file 的完整签名只在上图定义，其余新增方法如下。

```mermaid
classDiagram
    class VirtualFileSystem {
        +write_file(const Path& path, ByteView bytes, string& error) bool$
        +stat(const Path& path, FileEntry& output, string& error) bool$
        +list_directory(const Path& path, vector~FileEntry~& output, string& error) bool$
        +create_directory(const Path& path, string& error) bool$
        +copy_file(const Path& source, const Path& destination, string& error) bool$
        +move(const Path& source, const Path& destination, string& error) bool$
        +remove(const Path& path, string& error) bool$
    }
    class FileEntry {
        <<struct>>
        +Path path
        +FileKind kind = other
        +uint64_t size = 0
    }
    class FileKind {
        <<enumeration>>
        file
        directory
        symlink
        other
    }
    class Path
    class ByteView
    class PathUtils
    VirtualFileSystem ..> FileEntry : 输出独立快照
    VirtualFileSystem ..> Path : 调用期间借用
    VirtualFileSystem ..> ByteView : 写入期间借用
    VirtualFileSystem ..> PathUtils : 平台路径编码转换
    FileEntry *-- Path : 拥有路径值
    FileEntry ..> FileKind : 项目种类
```

FileEntry / FileKind 属于 `engine`，随核心文件模块定义；Path 为 filesystem::path，ByteView 为 span<const byte>，不是新类。实际读写、枚举、复制、移动、创建与删除使用 SDL3；SDL3 查询会跟随符号链接，因此只用标准库的跨平台 symlink_status 补充链接身份检查。全部静态操作同步完成，返回值 nodiscard；采用错误码式平台接口，不抛 C++ 异常。失败清空读／列表输出并将 FileEntry 还原为空状态；成功清空 error，不修改某个 VFS 实例的 error/last_error。

- 上图新增的原生文件操作使用明确的绝对路径；read_file / hash_file 继续接受工具调用方提供的原生路径，File 脚本绑定统一要求绝对路径。允许平台文件接口可访问的外部文件，权限检查由实际平台操作完成。路径不能包含原始 NUL。只读虚拟路径只开放平台已实现的操作，无法支持的枚举、写入或移动明确失败；不把虚拟资源路径回退为宿主工作目录下的文件。
- 文件层只处理字节，不校验 UTF-8 或配置语法。read_file / write_file 允许零字节；文本编码和 VM 上限由脚本边界处理，普通原生调用不承担 JSON 解析。
- stat 不跟随末端符号链接；list_directory 返回一层快照，不递归、不跟随子项链接，按子项名称的 UTF-8 字节序排序。普通文件记录字节大小，其他种类 size 为 0；发现权限／读取错误时整项失败，不返回无提示的半份目录。
- write_file 使用同目录临时文件写完并成功关闭后替换目标，失败清理临时文件并保留旧目标；仅接受普通文件或不存在的目标，不跟随目标符号链接写入。父目录必须存在。不承诺掉电持久性或跨多个文件的事务；平台不能保证该替换契约时失败。
- create_directory 只创建指定的一层目录；已有真实目录返回成功，父目录缺失或同名非目录失败。
- copy_file 只复制普通文件，不复制目录树，不覆盖已有目标，不跟随源符号链接；失败不留下半成品目标。move 移动单项文件／目录／符号链接，拒绝已有目标与跨文件系统移动，不隐式执行复制再删除；失败保留源。
- remove 删除单个普通文件、符号链接或空目录；删除链接只作用于链接本身。路径已不存在视为成功；非空目录与设备等特殊对象明确失败。没有递归删除、清空根目录或资源卸载副作用。
- 快照不保证下一次操作时文件仍存在；同一路径的并发修改由调用方串行化；存在性预检与 SDL 重命名之间不提供跨进程锁或原子拒绝覆盖保证。调用方负责目录扫描、重试和业务顺序，VFS 不建立文件监听或共享可变索引。

## 单次异步操作

```mermaid
classDiagram
    class FileRead {
        <<final>>
        +~FileRead()
        +status() FileReadStatus
        +take(ByteBuffer& output) bool
        +cancel() bool
        +error() FileError
        +last_error() string_view
    }
    class FileReadStatus {
        <<enumeration>>
        reading
        ready
        delivered
        failed
        cancelled
    }
    class FileError {
        <<enumeration>>
        none
        unavailable
        busy
        budget_exceeded
        invalid_location
        invalid_range
        io_failed
        cancelled
    }
    class ByteBuffer
    FileRead *-- ByteBuffer : 未领取的独占结果
    FileRead ..> FileReadStatus
    FileRead ..> FileError
```

FileRead 只能由 VFS 创建，调用方用 unique_ptr 独占；对象禁止复制/移动。查询 const noexcept，析构 noexcept，可能失败的返回 nodiscard。

reading → ready/failed/cancelled；ready → delivered（take）或 cancelled。take 只能成功一次，转移字节缓冲，不复制；失败输出空且保留原终态错误。cancel 对 reading/ready 生效，重复取消成功，对 delivered/failed 返回 false。销毁未领取操作即放弃；独立状态可以晚于 VFS 销毁，关闭后查询/取消/析构不解引用 VFS。

本设计用 FileRead 替代公开 function 回调式 read_async，便于上层按请求取消和接管所有权，不建立通用 Future、JobManager 或额外线程系统。
