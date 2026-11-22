# Google Play 发布

桌面编辑器使用可选插件 `editor/google_play` 为当前项目构建签名 AAB，并发布到该项目配置的 Google Play 轨道。启用 `editor/mcp` 时可通过编辑器 MCP 操作。Android 运行包只包含运行时内容。

## 项目与发布身份

项目的 `settings/editor-google-play.toml` 选择本机发布身份。应用名及方向属于项目。不同项目切换 `profile`，即可使用各自的包名、上传证书、版本和轨道。

```toml
[google_play]
profile = "sample"
label = "Sample"
orientation = "sensorLandscape"
```

每个身份单独保存在用户配置目录 `~/.config/engine/google-play/<profile>/`。可通过 `ENGINE_GOOGLE_PLAY_HOME` 指定团队使用的本机位置。该目录下的 `profile.json` 使用以下字段。签名文件与帐号文件的相对路径从身份目录解析。配置、密钥文件和服务帐号凭据均应仅对当前用户开放。

| 字段 | 用途 |
| --- | --- |
| `package`、`last_version` | 商店应用 ID 与已确认的四段版本，例如 `com.example.game` 和 `1.2.3.0` |
| `track`、`status` | 目标轨道与状态，例如 `internal` 和 `completed`，也支持 `draft` |
| `target_sdk` | 已安装的编译和目标 Android API 级别 |
| `keystore`、`alias` | 上传密钥库及其中的私钥条目 |
| `store_password`、`key_password` | 密钥库和私钥口令，仅传给签名子进程的环境 |
| `certificate_sha256` | 此应用上传证书 DER 数据的 64 位十六进制 SHA‑256 |
| `service_account` | 拥有目标应用和轨道发布权限的服务帐号 JSON |

本机 `toolchain.toml` 的 `[tools]` 表接受 `ANDROID_HOME`、`ANDROID_NDK_HOME`、`JAVA_HOME`、`VCPKG_ROOT`、`ENGINE_ASSET_BUILDER`、`ENGINE_BUNDLETOOL` 和可选的 `GRADLE_HOME`。也可以设置同名环境变量，环境变量优先。通过代理连接 Google 时可设置 HTTPS_PROXY。资源构建器为宿主的 Release `asset_build_app`，bundletool 使用 Google 发布的可执行 JAR。引擎 Android 预设和 Gradle 版本见[构建指南](../../building.md)。编辑器从可执行文件目录向上寻找已安装的引擎源码工具链，独立部署可通过绝对路径 `ENGINE_SDK_ROOT` 选择 SDK。默认使用 `uv` 运行带固定依赖版本的发布脚本，已有 Python 环境也可以通过 `ENGINE_GOOGLE_PLAY_PYTHON` 指定。

## 编辑器操作

| 命令 | 结果 |
| --- | --- |
| `start_google_play_release`，`action="inspect"` | 本机包名、轨道、候选版本及待核对提交状态 |
| `start_google_play_release`，`action="build"` | 重新编译当前项目的 Android 资源，构建、签名并验证 AAB |
| `start_google_play_release`，`action="publish"` | 读取 Play 版本，构建新包，上传、更新选定轨道、验证并提交 |
| `get_google_play_status` | 当前作业的运行状态、结果路径或错误；完成的发布包含版本和摘要 |

SDK 终端也可调用同一入口。示例构建当前项目的本机签名包。

```sh
uv run --script platforms/android/editor/google_play.py build --project projects/my-game
```

发布选择本机基线、成功回执、Play 已有 AAB、APK 以及各轨道中的最高版本代码。整包递增第三段，第四段固定为零，编码为 `major × 100000000 + minor × 1000000 + patch × 1000`。本地构建仅产生候选包。不同设备并行发布时，Play 的版本或编辑冲突会使该次操作失败；下一次发布重新读取商店。发布草稿时会保留已完成版本及现有草稿的说明。提交已完成版本时遇到未处理的草稿，或轨道包含分阶段发布、多版本发布，插件会返回限制并保留轨道配置。

每个包名有独立的本机发布锁。发布作业在项目缓存内单独保存 Android 源快照和导入缓存，可在桌面编辑器打开项目时构建。发布配置和凭据不进入快照。所有 Android 构建还共享构建目录锁，避免同时写入不同项目的资源。AAB 通过 `jarsigner` 签名，核对实际签名证书，并由 bundletool 验证结构、清单中的包名、版本及目标 SDK。上传时校验 Google 返回的包摘要。成功提交后更新版本回执。提交中断时保留待核对记录，再次运行 `publish` 会先按包摘要、版本、轨道和状态核实提交结果。

签名成品位于 `output/android/arm64/release/runtime/<package>/<package>-<versionCode>.aab`。构建日志在当前项目的 `cache/google-play/`。发布帐号 JSON 和密钥库的存储位置独立于运行包。[Google Play 编辑事务](https://developers.google.com/android-publisher/edits) · [Bundle 上传摘要](https://developers.google.com/android-publisher/api-ref/rest/v3/edits.bundles) · [Android 应用签名](https://developer.android.com/studio/publish/app-signing)
