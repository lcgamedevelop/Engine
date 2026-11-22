# Android 出包与 Google Play 发布

编辑器宿主借用任务域，向当前项目提供 APK、AAB 构建和发布命令。`project_package.read_settings` 从项目的 `settings/editor-android.toml` 读取分组配置。`build_android` 使用 `[android]` 和 `[android.version]` 生成应用清单，签名构建同时使用 `[android.signing]`，发布时由 `google_play` 使用独立的 `[google_play]` 表选择帐号和轨道。签名口令直接从配置读取，密钥库与服务帐号文件按配置路径定位。框架从同一次 Android 构建导出所选格式。[项目配置字段](../google-play.md)

```mermaid
classDiagram
    class GooglePlayPublisher {
        +GooglePlayPublisher(TaskSystem& tasks, path project, path sdk)
        +register_commands(EditorCommands& commands) bool
    }
    class google_play {
        <<module>>
        +release(project, action, output_format) dict
        +signed_packages(project, android, env, name, code, formats) dict
    }
    class project_package {
        <<module>>
        +read_settings(path) dict
        +next_version(version, maximum) tuple
    }
    class build_android {
        <<module>>
    }
    class GooglePlayEdits {
        <<external>>
    }
    GooglePlayPublisher --> TaskSystem : borrows editor domain
    GooglePlayPublisher --> EditorCommands : registers start and status
    GooglePlayPublisher ..> google_play : runs SDK worker for opened project
    google_play ..> project_package : uses project settings and versions
    build_android ..> project_package : reads project manifest inputs
    google_play ..> build_android : builds Android bundle once
    google_play ..> GooglePlayEdits : uploads AAB to project track
```
