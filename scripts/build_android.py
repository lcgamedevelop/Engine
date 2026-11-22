"""Build native libraries with root CMake Presets, then package an APK or Play App Bundle with the SDL3 Activity framework."""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import zipfile

from artifact_output import validate_output
from build_lock import exclusive_build

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "platforms/android/editor"))
from project_package import read_settings


def require_directory(name):
    value = os.environ.get(name, "")
    if not value or not Path(value).is_dir():
        raise ValueError(f"Set {name} to an existing directory before retrying.")
    return Path(value).resolve()


def gradle_command(java):
    # 可复用已安装的 Gradle，包括 Unity 提供的纯 lib 分发；不记录本机路径。
    configured = os.environ.get("GRADLE_HOME", "")
    if configured:
        gradle_root = Path(configured)
        executable = gradle_root / "bin" / ("gradle.bat" if os.name == "nt" else "gradle")
        if executable.is_file():
            return [str(executable)]
        launcher = gradle_root / "lib" / "gradle-launcher-9.8.2.jar"
        if launcher.is_file():
            return [str(java), "-classpath", str(launcher), "org.gradle.launcher.GradleMain"]
        raise ValueError("GRADLE_HOME does not contain Gradle 9.8.2; correct it or unset it to use the wrapper.")
    wrapper = ROOT / "platforms/android" / ("gradlew.bat" if os.name == "nt" else "gradlew")
    return [str(wrapper)]


def run(command):
    print("Running: ", subprocess.list2cmdline(command), flush=True)
    subprocess.run(command, cwd=ROOT, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", choices=("debug", "release"), default="debug")
    parser.add_argument("--backend", choices=("quickjs", "v8"), default="quickjs")
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 1, 8))
    parser.add_argument(
        "--project", type=Path, help="Project to compile and package in the APK; defaults to projects/showcase"
    )
    parser.add_argument(
        "--asset-builder",
        type=Path,
        help="Host asset_build_app for incremental --target android compilation; otherwise an Android catalog must already exist",
    )
    parser.add_argument("--skip-native", action="store_true", help="Package native libraries already produced by CMake")
    parser.add_argument("--unsigned-bundle", type=Path, help="Build an unsigned release bundle into a staging file")
    parser.add_argument("--package-settings", type=Path, help="Source project settings for an isolated Android asset build")
    parser.add_argument("--version-name")
    parser.add_argument("--version-code", type=int)
    args = parser.parse_args()
    if args.unsigned_bundle and (args.config != "release" or args.skip_native or not args.project
                                 or not args.asset_builder or not args.version_name or not args.version_code):
        parser.error("A bundle requires a release project build, asset builder and a resolved Android version.")
    if not args.unsigned_bundle and (args.version_name or args.version_code is not None or args.package_settings):
        parser.error("Local installation builds use the current project's package settings.")
    if args.jobs < 1:
        parser.error("--jobs must be greater than 0")
    if args.skip_native and args.asset_builder is not None:
        parser.error("--skip-native reuses built resources and cannot use an asset builder.")
    asset_builder = args.asset_builder.resolve() if args.asset_builder is not None else None
    if asset_builder is not None and (
        not asset_builder.is_file() or not os.access(asset_builder, os.X_OK)
    ):
        parser.error("--asset-builder must point to an executable host asset_build_app; build the desktop asset tool first.")
    project = args.project or ROOT / "projects/showcase"
    settings = read_settings(args.package_settings or project / "settings/editor-android.toml")
    require_directory("ANDROID_HOME")
    ndk_root = require_directory("ANDROID_NDK_HOME")
    properties = (ndk_root / "source.properties").read_text(encoding="utf-8")
    revision = next(
        (
            line.partition("=")[2].strip()
            for line in properties.splitlines()
            if line.partition("=")[0].strip() == "Pkg.Revision"
        ),
        "",
    )
    if revision != "30.0.16248370":
        raise ValueError("Android requires NDK 30.0.16248370; correct ANDROID_NDK_HOME.")
    java_root = require_directory("JAVA_HOME")
    java = java_root / "bin" / ("java.exe" if os.name == "nt" else "java")
    if not java.is_file():
        raise ValueError("JAVA_HOME does not contain Java; prepare JDK 17.")
    gradle = gradle_command(java)
    version = subprocess.check_output(gradle + ["--version"], cwd=ROOT, text=True)
    if "Gradle 9.8.2" not in version:
        raise ValueError("Android requires Gradle 9.8.2; correct GRADLE_HOME or use the wrapper.")
    with exclusive_build(ROOT / "build/android-gradle/build.lock"):
        return package(args, project, settings, asset_builder, gradle)


def package(args, project, android, asset_builder, gradle):
    preset = "android-arm64-" + ("v8-" if args.backend == "v8" else "") + args.config
    if args.skip_native:
        cache = (ROOT / "build" / preset / "CMakeCache.txt").read_text(encoding="utf-8")
        cached = next((v.partition("=")[2] for v in cache.splitlines()
                       if v.startswith("ENGINE_ANDROID_PROJECT:PATH=")), "")
        source = Path(cached).resolve() if cached else ROOT / "projects/showcase"
        if source != project.resolve() and not source.is_relative_to(project.resolve() / "cache"):
            raise ValueError("The cached Android resources belong to another project. Run a full project build.")
    if not args.skip_native:
        require_directory("VCPKG_ROOT")
        cmake = shutil.which("cmake")
        if not cmake:
            raise ValueError("CMake was not found; run the environment setup script first.")
        run(
            [
                cmake,
                "--preset",
                preset,
                "-DENGINE_V8_JOBS=" + str(args.jobs),
                "-DENGINE_ANDROID_PROJECT=" + str(project.resolve()),
                "-DENGINE_ANDROID_ASSET_BUILDER=" + (str(asset_builder) if asset_builder else ""),
            ]
        )
        run([cmake, "--build", "--preset", preset + "-prepare"])
        run([cmake, "--build", "--preset", preset, "--parallel", str(args.jobs)])
    source = (
        ROOT
        / "build/android-gradle"
        / args.config
        / "outputs/apk"
        / args.config
        / ("app-" + args.config + ".apk")
    )
    options = [
        "-PengineApplicationId=" + android["package"],
        "-PengineVersionName=" + (args.version_name or android["version_name"]),
        "-PengineVersionCode=" + str(args.version_code or android["version_code"]),
        "-PengineMinSdk=" + str(android["min_sdk"]),
        "-PengineTargetSdk=" + str(android["target_sdk"]),
        "-PengineLabel=" + android["label"],
        "-PengineOrientation=" + android["orientation"],
        "-PengineFullscreen=" + str(android.get("fullscreen", False)).lower(),
    ]
    task = ":app:assemble" + args.config.title()
    if args.unsigned_bundle:
        source = ROOT / "build/android-gradle/play/outputs/bundle/release/app-release.aab"
        task = ":app:bundleRelease"
        options.append("-PenginePlayBundle=true")
    # APK 增量更新可能保留被替换的 .so 占用的空洞；仅重建最终包，保留所有编译缓存。
    source.unlink(missing_ok=True)
    run(
        gradle
        + [
            "--no-daemon",
            "--build-cache",
            "--project-cache-dir",
            str(ROOT / "build/android-gradle/cache"),
            "-p",
            str(ROOT / "platforms/android"),
            "-PengineConfiguration=" + args.config,
            "-PengineScriptBackend=" + args.backend,
            *options, task,
        ]
    )
    if not source.is_file():
        raise ValueError(f"Gradle did not produce the expected Android package: {source}")
    prefix = "base/" if args.unsigned_bundle else ""
    with zipfile.ZipFile(source) as archive:
        native = sorted(name for name in archive.namelist() if name.endswith(".so"))
        if native != [prefix + "lib/arm64-v8a/libmain.so"]:
            raise ValueError(f"Android delivery must contain only libmain.so; found: {native}")
        if prefix + "assets/catalog.bin" not in archive.namelist():
            raise ValueError("Android package is missing the separate runtime resource catalog")
    if args.unsigned_bundle:
        with args.unsigned_bundle.open("xb") as target, source.open("rb") as bundle:
            shutil.copyfileobj(bundle, target)
        return 0
    output = validate_output(ROOT / "output/android/arm64" / args.config / "runtime",
                             ROOT / "output", [source, project.resolve()])
    output.mkdir(parents=True, exist_ok=True)
    destination = output / (android["artifact_name"] + ".apk")
    with tempfile.TemporaryDirectory(dir=output) as stage:
        staged_apk = Path(stage) / destination.name
        shutil.copy2(source, staged_apk)
        os.replace(staged_apk, destination)
    print(f"APK created: {destination}")
    print("Debug and Release APKs both use the local debug signature and are intended only for installation tests.")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"Android packaging did not complete: {error}", file=sys.stderr)
        sys.exit(1)
