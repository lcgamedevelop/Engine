# Android 出包与 Google Play 发布

桌面编辑器插件 `editor/google_play` 为当前项目导出可直接安装的签名 APK、供 Google Play 分发的 AAB，或将 AAB 发布到项目指定轨道。启用 `editor/mcp` 后可在编辑器调用这些操作。每次出包都重新读取当前项目设置。

## 填写项目设置

每个项目维护一个 `settings/editor-android.toml`。本地安装测试只需应用 ID、显示名、版本名和版本码基线。横屏游戏可再指定方向。需要签名出包时填写上传密钥，需要发布时填写商店轨道和服务帐号。下面是一个横屏游戏的内部测试配置。

```toml
[android]
package = "com.example.adventure"
label = "Adventure"
orientation = "sensorLandscape"

[android.version]
name = "1.2.3"
code = 10203

[android.signing]
keystore = "private/android/release/upload.jks"
alias = "upload"
store_password = "<keystore password>"
key_password = "<key password>"

[google_play]
track = "internal"
status = "completed"
service_account = "private/android/release/play-account.json"
```

| 分组 | 填写内容 |
| --- | --- |
| `[android]` | `package` 是应用 ID，`label` 是安装后显示的名称。横屏或竖屏由 `orientation` 指定，如 `sensorLandscape`、`portrait`。省略时跟随系统和 SDL 的默认方向策略。 |
| `[android.version]` | `name` 是玩家看到的版本名。`code` 是版本码基线，新应用可从 0 起步，安装包版本码最低为 1。默认保留版本名，签名候选包在已知最高版本码上加一。已上架应用如有按段编码的规则，可同时填写各数字段的 `weights` 与从 1 起计的 `increment_component`，递增时后续段归零。 |
| `[android.signing]` | `keystore`、`alias`、`store_password`、`key_password` 分别指定上传密钥库、别名和两项口令。框架从实际密钥取得证书并校验成品。已有商店应用可用 `certificate_sha256` 锁定上传证书，选填 `profile` 可为回执中的签名身份命名。 |
| `[google_play]` | `track` 指定轨道，`status` 取 `draft` 或 `completed`，`service_account` 指向有发布权限的服务帐号密钥 JSON。 |

只有 `keystore` 和 `service_account` 填文件路径。两者相对于这份 TOML 所在的 `settings/` 目录解析，文件放在当前项目的 `settings/private/` 下，可使用不同子目录。用于发布的项目配置含实际口令，保存在项目本机；签名工具读取这些值，日志、回执与运行包只包含各自需要的非敏感数据。

## 框架默认值

| 项目可省略的参数 | 生效规则 |
| --- | --- |
| `fullscreen` | 游戏默认沉浸式全屏。Android 启动主题与 SDL 窗口使用同一选择，系统栏可用手势临时唤出。需要常驻系统栏的应用可填 `false`。世界画面铺满可绘制区域，HUD 可通过 `Window.GetSafeArea()` 对齐安全区域。 |
| `formats`、`artifact_name` | 编辑器签名构建默认同时导出 APK 和 AAB。文件名前缀取显示名的小写英文及数字，词间用连字符，无法从显示名生成时取包名。项目需要固定文件名或单一格式时可覆盖。 |
| `min_sdk`、`target_sdk` | 引擎支持的最低 API 为 28，目标与编译基线为 36。项目可提高最低 API 或显式指定目标 API。省略目标时取 36 与最低 API 的较大值。编译 API 由框架取 36 与有效目标 API 的较大值，构建机需安装对应 SDK Platform。 |

Unity 将方向、显示范围、应用身份和版本放在 Android Player Settings，另由构建选项选择 AAB。Cocos Creator 的 Android 构建面板也按项目填写包名、方向、密钥与产物选项。本引擎沿用这种职责划分，由编辑器将生效值统一传给 Gradle 和原生窗口。[Unity Android Player Settings](https://docs.unity.com/en-us/engine/6000.0/manual/platform-specific/android/getting-started/class-player-settings) · [Unity Android 构建选项](https://docs.unity.com/en-us/engine/6000.0/manual/platform-specific/android/building-and-delivering/build-settings) · [Cocos Creator 3.8 Android 构建选项](https://docs.cocos.com/creator/3.8/manual/en/editor/publish/android/build-options-android.html)

沉浸模式下系统保留手势唤出系统栏。应用显示版本与商店递增版本码分别遵守 Android 的 `versionName` 和 `versionCode` 约定。[Android 沉浸模式](https://developer.android.com/develop/ui/views/layout/immersive) · [Android 应用版本](https://developer.android.com/studio/publish/versioning)

## 出包与上传

| 编辑器命令 | 项目收到的结果 |
| --- | --- |
| `start_google_play_release`，`action="inspect"` | 包名、生效的方向、全屏、三项 SDK、文件名前缀、输出格式、候选版本及待核对提交状态。配置了签名身份或商店目标时也返回其名称。 |
| `start_google_play_release`，`action="build"` | 按项目的 `formats` 输出成品。可临时指定 `format="apk"` 或 `format="aab"` |
| `start_google_play_release`，`action="publish"` | 核对商店版本，构建签名 AAB，上传后验证并提交到项目指定轨道 |
| `get_google_play_status` | 作业状态、版本和 `artifacts` 中各成品的路径，发布成功时还包含包摘要 |

SDK 终端也可运行同一入口。下例仅生成可安装的 APK。

```sh
uv run --script platforms/android/editor/google_play.py build --project projects/my-game --format apk
```

APK 和 AAB 共用一次 Android 资源和原生构建。AAB 经 jarsigner 签名和 bundletool 验证，安装用 APK 从同一 bundle 生成并通过 apksigner 验签。插件核对包名、版本、名称、方向、全屏设置、三项 SDK 和上传证书。成品位于 `output/android/arm64/release/runtime/<package>/<code>/<artifact_name>-<code>.apk` 或同名 `.aab`，构建日志在项目的 `cache/google-play/`。

本地构建使用项目版本基线和最近的成功回执，可以重复出同一候选版本。发布同时读取 Play 已有 AAB、APK 和各轨道中的最高代码，再按项目规则递增。跨设备并发发布遇到商店版本冲突时，重新发布会读取最新版本。草稿保留已有正式版本和草稿说明。遇到分阶段发布、多版本发布或需要先处理的草稿，插件保留轨道状态并返回原因。

每个包名有独立的构建锁。项目在自己的缓存编译 Android 资源，桌面编辑器可以同时打开项目。成功回执和待核对提交保存在项目的 `settings/private/android/.releases/<package>/`。提交中断后再次发布会按摘要、版本、轨道和状态核对结果。框架使用 Google 官方编辑事务和 Android 签名工具。[Google Play 编辑事务](https://developers.google.com/android-publisher/edits) · [Android 应用签名](https://developer.android.com/studio/publish/app-signing)

## 安装机器工具

每台构建机准备 Android SDK、NDK、JDK、vcpkg、宿主 Release 的 `asset_build_app` 和 Google bundletool 一次。可用环境变量 `ANDROID_HOME`、`ANDROID_NDK_HOME`、`JAVA_HOME`、`VCPKG_ROOT`、`ENGINE_ASSET_BUILDER`、`ENGINE_BUNDLETOOL` 和可选的 `GRADLE_HOME`，也可在 `~/.config/engine/android-toolchain.toml` 的 `[tools]` 表设置。设置 `ENGINE_ANDROID_TOOLCHAIN` 可指定机器工具路径配置文件。编辑器从自身位置寻找引擎 SDK，独立部署使用 `ENGINE_SDK_ROOT`。默认用 `uv` 启动固定依赖的发布程序，也可设置 `ENGINE_GOOGLE_PLAY_PYTHON`。[工具链版本与构建指南](../../building.md)
