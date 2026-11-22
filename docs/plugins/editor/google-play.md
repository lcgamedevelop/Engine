# Android 出包与 Google Play 发布

桌面编辑器插件 `editor/google_play` 为当前项目导出可直接安装的签名 APK、供 Google Play 分发的 AAB，或将 AAB 发布到项目指定轨道。启用 `editor/mcp` 后可在编辑器调用这些操作。每次出包都重新读取当前项目设置。

## 填写项目设置

每个项目只维护一个 `settings/editor-android.toml`。Android 应用、版本与签名各有自己的表，Google Play 使用同文件中的独立表。下面的项目默认生成 APK 和 AAB，并将发布目标设为内部测试。

```toml
[android]
package = "com.example.adventure"
label = "Adventure"
artifact_name = "adventure"
orientation = "sensorLandscape"
fullscreen = true
formats = ["apk", "aab"]
min_sdk = 28
target_sdk = 36

[android.version]
name = "1.2.3"
code = 10203
weights = [10000, 100, 1]
increment_component = 3

[android.signing]
profile = "release"
keystore = "private/android/release/upload.jks"
alias = "upload"
certificate_sha256 = "<64 hex digits from the upload certificate>"
store_password = "<keystore password>"
key_password = "<key password>"

[google_play]
track = "internal"
status = "completed"
service_account = "private/android/release/play-account.json"
```

| 分组 | 填写内容 |
| --- | --- |
| `[android]` | `package` 是应用 ID，`label` 是设备上显示的名称，`artifact_name` 是成品文件名前缀。`orientation`、`fullscreen` 控制方向与沉浸式全屏，全屏默认关闭。用 `formats` 选择 APK、AAB 或两者，用 `min_sdk`、`target_sdk` 指定 API 版本。当前最低支持 API 28。 |
| `[android.version]` | `name` 和 `code` 填写起始版本，接续已上架应用时填写商店已有版本。`weights` 与名称的数字分段一一对应，`increment_component` 从 1 开始计数，递增时后面的段归零。 |
| `[android.signing]` | 签名出包时填写。选填的 `profile` 是显示在回执中的签名身份名称。`keystore` 指向上传密钥库，`alias` 指定其中的私钥条目。`certificate_sha256` 填写上传证书的 64 位十六进制摘要。`store_password` 和 `key_password` 分别直接填写库口令和私钥口令。 |
| `[google_play]` | 上传商店时填写。`track` 指定轨道，`status` 取 `draft` 或 `completed`，`service_account` 指向有该应用发布权限的服务帐号密钥 JSON。本地签名构建可省略整段。 |

只有 `keystore` 和 `service_account` 填文件路径。两者相对于这份 TOML 所在的 `settings/` 目录解析，文件放在当前项目的 `settings/private/` 下，可使用不同子目录。用于发布的项目配置含实际口令，保存在项目本机；签名工具读取这些值，日志、回执与运行包只包含各自需要的非敏感数据。

## 出包与上传

| 编辑器命令 | 项目收到的结果 |
| --- | --- |
| `start_google_play_release`，`action="inspect"` | 包名、profile、默认格式、候选版本、待核对提交状态 |
| `start_google_play_release`，`action="build"` | 按项目的 `formats` 输出成品。可临时指定 `format="apk"` 或 `format="aab"` |
| `start_google_play_release`，`action="publish"` | 核对商店版本，构建签名 AAB，上传后验证并提交到项目指定轨道 |
| `get_google_play_status` | 作业状态、版本和 `artifacts` 中各成品的路径，发布成功时还包含包摘要 |

SDK 终端也可运行同一入口。下例仅生成可安装的 APK。

```sh
uv run --script platforms/android/editor/google_play.py build --project projects/my-game --format apk
```

APK 和 AAB 共用一次 Android 资源和原生构建。AAB 经 jarsigner 签名和 bundletool 验证，安装用 APK 从同一 bundle 生成并通过 apksigner 验签。插件核对包名、版本、屏幕方向、名称、最低及目标 SDK 和预期证书。成品位于 `output/android/arm64/release/runtime/<package>/<code>/<artifact_name>-<code>.apk` 或同名 `.aab`，构建日志在项目的 `cache/google-play/`。

本地构建使用项目版本基线和最近的成功回执，可以重复出同一候选版本。发布同时读取 Play 已有 AAB、APK 和各轨道中的最高代码，再按项目规则递增。跨设备并发发布遇到商店版本冲突时，重新发布会读取最新版本。草稿保留已有正式版本和草稿说明。遇到分阶段发布、多版本发布或需要先处理的草稿，插件保留轨道状态并返回原因。

每个包名有独立的构建锁。项目在自己的缓存编译 Android 资源，桌面编辑器可以同时打开项目。成功回执和待核对提交保存在项目的 `settings/private/android/.releases/<package>/`。提交中断后再次发布会按摘要、版本、轨道和状态核对结果。框架使用 Google 官方编辑事务和 Android 签名工具。[Google Play 编辑事务](https://developers.google.com/android-publisher/edits) · [Android 应用签名](https://developer.android.com/studio/publish/app-signing)

## 安装机器工具

每台构建机准备 Android SDK、NDK、JDK、vcpkg、宿主 Release 的 `asset_build_app` 和 Google bundletool 一次。可用环境变量 `ANDROID_HOME`、`ANDROID_NDK_HOME`、`JAVA_HOME`、`VCPKG_ROOT`、`ENGINE_ASSET_BUILDER`、`ENGINE_BUNDLETOOL` 和可选的 `GRADLE_HOME`，也可在 `~/.config/engine/android-toolchain.toml` 的 `[tools]` 表设置。设置 `ENGINE_ANDROID_TOOLCHAIN` 可指定机器工具路径配置文件。编辑器从自身位置寻找引擎 SDK，独立部署使用 `ENGINE_SDK_ROOT`。默认用 `uv` 启动固定依赖的发布程序，也可设置 `ENGINE_GOOGLE_PLAY_PYTHON`。[工具链版本与构建指南](../../building.md)
