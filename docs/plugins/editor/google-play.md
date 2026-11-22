# Android 出包与 Google Play 发布

桌面编辑器插件 `editor/google_play` 为当前项目导出可直接安装的签名 APK、供 Google Play 分发的 AAB，或将 AAB 发布到项目指定轨道。启用 `editor/mcp` 后可在编辑器调用这些操作。每次出包都重新读取当前项目设置。

## 填写项目设置

在项目的 `settings/editor-android.toml` 中填写一个 `[android]` 表。以下示例使用三段版本号，默认同时导出两种格式。

```toml
[android]
profile = "release"
package = "com.example.adventure"
label = "Adventure"
artifact_name = "adventure"
orientation = "sensorLandscape"
fullscreen = true
formats = ["apk", "aab"]
min_sdk = 28
target_sdk = 36

version_name = "1.2.3"
version_code = 10203
version_weights = [10000, 100, 1]
increment_component = 3

keystore = "upload.jks"
alias = "upload"
certificate_sha256 = "<64 hex digits from the upload certificate>"
password_file = "passwords.json"

play_track = "internal"
play_status = "completed"
service_account = "play-account.json"
```

| 字段 | 填写内容 |
| --- | --- |
| `profile` | 当前项目的签名目录名。示例对应 `settings/private/android/release/` |
| `package`、`label`、`artifact_name` | 商店应用 ID、设备显示名称和产物文件名前缀 |
| `orientation`、`formats` | Android 屏幕方向和默认成品格式，可选 APK、AAB 或两者 |
| `fullscreen` | 默认为 `false`。设为 `true` 时开启沉浸式全屏，内容延伸至刘海两侧。HUD 可读取窗口安全区 |
| `min_sdk`、`target_sdk` | 项目支持的最低与目标 Android API。引擎 ARM64 运行时最低支持 API 28 |
| `version_name`、`version_code` | 起始版本名称和代码。接续已上架应用时填写商店已有版本 |
| `version_weights`、`increment_component` | 各段的代码权重和每次递增的段序号，首段为 1。低位清零，超过某一段的容量会进位 |
| `keystore`、`alias`、`certificate_sha256` | 当前 profile 中的上传密钥库、私钥条目和预期证书 SHA‑256 |
| `password_file` | 当前 profile 中的私有 JSON，字段为 `store_password` 和 `key_password` |
| `play_track`、`play_status`、`service_account` | 发布轨道、`draft` 或 `completed`、当前 profile 中的 Google 服务帐号 JSON。仅做本地包的项目可以省略这三项 |

已有应用沿用原来的应用 ID、上传证书和版本基线。另一个项目填写自己的设置并放入自己的签名文件即可。密钥、口令和帐号凭据在项目的 `settings/private/` 下私存，此目录应排除版本管理和运行包。

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
