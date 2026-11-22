# /// script
# requires-python = ">=3.11"
# dependencies = ["google-api-python-client==2.197.0", "google-auth==2.53.0", "PySocks==1.7.1"]
# ///
"""Build or publish the open Engine project's signed Android App Bundle."""

import argparse
import base64
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import tomllib
from urllib.request import getproxies
import xml.etree.ElementTree as ET
import zipfile

SDK = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(SDK / "scripts"))
from build_lock import exclusive_build
from artifact_output import plain_path, staged_output
from prepare_android_assets import asset_files
from project_package import (
    FORMATS,
    ORIENTATIONS,
    android_snapshot_path,
    next_version,
    private_file,
    read_settings,
)

ANDROID = "{http://schemas.android.com/apk/res/android}"


def atomic_json(path, data):
    with tempfile.NamedTemporaryFile(
        mode="w", dir=path.parent, encoding="utf-8", delete=False
    ) as f:
        temp = Path(f.name)
        try:
            json.dump(data, f)
            f.flush()
            os.fsync(f.fileno())
        except BaseException:
            temp.unlink(missing_ok=True)
            raise
    try:
        os.replace(temp, path)
    finally:
        temp.unlink(missing_ok=True)


def toolchain():
    config = Path(
        os.environ.get(
            "ENGINE_ANDROID_TOOLCHAIN", Path.home() / ".config/engine/android-toolchain.toml"
        )
    )
    paths = tomllib.loads(config.read_text()).get("tools", {}) if config.exists() else {}
    env = dict(os.environ)
    for name in (
        "ANDROID_HOME",
        "ANDROID_NDK_HOME",
        "JAVA_HOME",
        "VCPKG_ROOT",
        "GRADLE_HOME",
        "ENGINE_ASSET_BUILDER",
        "ENGINE_BUNDLETOOL",
        "HTTPS_PROXY",
        "HTTP_PROXY",
        "NO_PROXY",
    ):
        if name not in env and name in paths:
            env[name] = paths[name]
    required = (
        "ANDROID_HOME",
        "ANDROID_NDK_HOME",
        "JAVA_HOME",
        "VCPKG_ROOT",
        "ENGINE_ASSET_BUILDER",
        "ENGINE_BUNDLETOOL",
    )
    if any(not env.get(n) or not Path(env[n]).exists() for n in required):
        raise ValueError(
            "Configure the Android SDK, NDK, JDK, vcpkg, asset builder and bundletool in the host toolchain."
        )
    return env


def android_project_snapshot(project, destination):
    # The active desktop editor owns its platform-specific import cache.
    # Keep Android's generated GUID catalog in a stable, separate project workspace.
    destination.mkdir(parents=True, exist_ok=True)
    (destination / "cache").mkdir(exist_ok=True)
    source = project / "assets"

    def inputs():
        result = {}
        for path in asset_files(source):
            stat = path.stat()
            result[path.relative_to(source)] = (stat.st_size, stat.st_mtime_ns, stat.st_ctime_ns)
        return result

    files = inputs()
    with staged_output(destination / "assets") as stage:
        for relative in files:
            path = stage / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source / relative, path)
        if inputs() != files:
            raise ValueError(
                "Project assets changed during the Android build snapshot. Retry the build."
            )
    source_settings = project / "settings"

    def settings_files():
        return {
            p.name: p.read_bytes()
            for p in source_settings.iterdir()
            if p.name in ("editor-import.toml", "editor-resource-build.toml")
            or (p.name.startswith("runtime-") and p.suffix == ".toml")
        }

    selected = settings_files()
    with staged_output(destination / "settings") as stage:
        for name, contents in selected.items():
            (stage / name).write_bytes(contents)
        typescript = source_settings / "typescript"
        if typescript.is_dir():
            # npm bin links are resolved into the local copy. The compiler and its
            # declarations still come from the same configured tooling as other builds.
            shutil.copytree(typescript, stage / "typescript")
        if settings_files() != selected:
            raise ValueError(
                "Runtime or import settings changed during the Android build snapshot."
            )
    return destination


def signer_fingerprint(pem_output):
    match = re.search(
        rb"-----BEGIN CERTIFICATE-----(.*?)-----END CERTIFICATE-----", pem_output, re.S
    )
    if not match:
        raise ValueError("The signing tool did not return a certificate.")
    return hashlib.sha256(base64.b64decode(match[1])).hexdigest()


@contextmanager
def bundletool_password_args(project, signing):
    with tempfile.TemporaryDirectory(dir=project / "cache/google-play") as directory:
        args = []
        for field, option in (("store_password", "--ks-pass"), ("key_password", "--key-pass")):
            path = Path(directory) / field
            with os.fdopen(
                os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600), "w", encoding="utf-8"
            ) as stream:
                stream.write(signing[field])
            args.append(option + "=file:" + str(path))
        yield args


def signed_packages(project, android, env, name, code, formats):
    if "signing" not in android:
        raise ValueError("Configure [android.signing] to build signed Android packages.")
    signing = android["signing"]
    package = android["package"]
    keystore = private_file(project, signing["keystore"])
    output = SDK / "output/android/arm64/release/runtime" / package / str(code)
    output.parent.mkdir(parents=True, exist_ok=True)
    logs = project / "cache/google-play"
    logs.mkdir(parents=True, exist_ok=True)
    logfile = logs / f"build-{code}.log"
    jdk = Path(env["JAVA_HOME"]) / "bin"
    suffix = ".exe" if os.name == "nt" else ""
    keytool = str(jdk / ("keytool" + suffix))
    jarsigner = str(jdk / ("jarsigner" + suffix))
    secret_env = {
        **env,
        "ENGINE_UPLOAD_STORE_PASSWORD": signing["store_password"],
        "ENGINE_UPLOAD_KEY_PASSWORD": signing["key_password"],
    }
    store = ["-keystore", str(keystore), "-storepass:env", "ENGINE_UPLOAD_STORE_PASSWORD"]
    cert = subprocess.run(
        [keytool, "-exportcert", *store, "-alias", signing["alias"]],
        env=secret_env,
        capture_output=True,
        check=True,
    ).stdout
    fingerprint = hashlib.sha256(cert).hexdigest()
    pinned = signing.get("certificate_sha256")
    if pinned is not None and pinned.lower() != fingerprint:
        raise ValueError("The keystore certificate does not match this app's upload certificate.")
    android_project = android_project_snapshot(project, android_snapshot_path(project, package))
    with tempfile.TemporaryDirectory(dir=output.parent) as tmp, logfile.open("w") as log:
        stage = Path(tmp)
        unsigned, signed = stage / "unsigned.aab", stage / "signed.aab"

        def run(args, *, signing=False):
            if subprocess.run(
                args, env=secret_env if signing else env, stdout=log, stderr=subprocess.STDOUT
            ).returncode:
                raise ValueError(
                    f"Android bundle step failed ({Path(args[0]).name}); see {logfile}"
                )

        run(
            [
                sys.executable,
                str(SDK / "scripts/build_android.py"),
                "--config",
                "release",
                "--project",
                str(android_project),
                "--asset-builder",
                env["ENGINE_ASSET_BUILDER"],
                "--unsigned-bundle",
                str(unsigned),
                "--package-settings",
                str(project / "settings/editor-android.toml"),
                "--version-name",
                name,
                "--version-code",
                str(code),
            ]
        )
        run(
            [
                jarsigner,
                *store,
                "-keypass:env",
                "ENGINE_UPLOAD_KEY_PASSWORD",
                "-signedjar",
                str(signed),
                str(unsigned),
                signing["alias"],
            ],
            signing=True,
        )
        run([jarsigner, "-verify", str(signed)])
        certs = subprocess.check_output(
            [keytool, "-printcert", "-rfc", "-jarfile", str(signed)], env=env
        )
        if signer_fingerprint(certs) != fingerprint:
            raise ValueError("The signed bundle does not have the configured upload certificate.")
        java = str(jdk / ("java" + suffix))
        bundletool = [java, "-jar", env["ENGINE_BUNDLETOOL"]]
        run(bundletool + ["validate", "--bundle", str(signed)])
        xml = subprocess.check_output(
            bundletool + ["dump", "manifest", "--bundle", str(signed)], env=env
        )
        manifest = ET.fromstring(xml)
        target = manifest.find("uses-sdk")
        application = manifest.find("application")
        fullscreen = manifest.find(
            "application/meta-data[@" + ANDROID + "name='SDL_ENV.ENGINE_ANDROID_FULLSCREEN']"
        )
        activity = manifest.find(
            "application/activity/intent-filter/category[@"
            + ANDROID
            + "name='android.intent.category.LAUNCHER']/../.."
        )
        if (
            (
                manifest.get("package"),
                manifest.get(ANDROID + "versionName"),
                manifest.get(ANDROID + "versionCode"),
            )
            != (package, name, str(code))
            or manifest.get(ANDROID + "compileSdkVersion") != str(android["compile_sdk"])
            or target is None
            or target.get(ANDROID + "targetSdkVersion") != str(android["target_sdk"])
            or target.get(ANDROID + "minSdkVersion") != str(android["min_sdk"])
            or application is None
            or application.get(ANDROID + "label") != android["label"]
            or fullscreen is None
            or fullscreen.get(ANDROID + "value") != str(android["fullscreen"]).lower()
            or activity is None
            or activity.get(ANDROID + "screenOrientation")
            not in (android["orientation"], str(ORIENTATIONS[android["orientation"]]))
        ):
            raise ValueError("The signed bundle's manifest does not match the release profile.")
        files = {"aab": signed}
        if "apk" in formats:
            apk = stage / "signed.apk"
            apks = stage / "device.apks"
            with bundletool_password_args(project, signing) as passwords:
                run(
                    bundletool
                    + [
                        "build-apks",
                        "--mode=universal",
                        "--bundle=" + str(signed),
                        "--output=" + str(apks),
                        "--ks=" + str(keystore),
                        "--ks-key-alias=" + signing["alias"],
                        *passwords,
                    ]
                )
            with (
                zipfile.ZipFile(apks) as archive,
                archive.open("universal.apk") as source,
                apk.open("wb") as out,
            ):
                shutil.copyfileobj(source, out)
            apksigner = Path(env["ANDROID_HOME"]) / "build-tools/37.0.0/lib/apksigner.jar"
            verified = subprocess.check_output(
                [java, "-jar", str(apksigner), "verify", "--print-certs-pem", str(apk)], env=env
            )
            if signer_fingerprint(verified) != fingerprint:
                raise ValueError("The APK does not match the configured signing certificate.")
            files["apk"] = apk
        stem = f"{android['artifact_name']}-{code}"
        with staged_output(output) as completed:
            for other in FORMATS:
                previous = output / (stem + "." + other)
                if other not in formats and previous.is_file():
                    shutil.copy2(previous, completed / previous.name)
            for fmt in formats:
                os.replace(files[fmt], completed / (stem + "." + fmt))
    return {fmt: str(output / (stem + "." + fmt)) for fmt in formats}


def google_publisher(service_account_path, env):
    import httplib2
    from google.oauth2 import service_account
    from google_auth_httplib2 import AuthorizedHttp
    from googleapiclient.discovery import build

    account = json.loads(service_account_path.read_text())
    if account.get("type") != "service_account" or account.get("token_uri") not in (
        "https://oauth2.googleapis.com/token",
        "https://accounts.google.com/o/oauth2/token",
    ):
        raise ValueError("Select a Google service account with an official OAuth token endpoint.")
    credentials = service_account.Credentials.from_service_account_info(
        account, scopes=["https://www.googleapis.com/auth/androidpublisher"]
    )
    proxies = getproxies()
    proxy = env.get("HTTPS_PROXY") or env.get("https_proxy") or proxies.get("https")
    http = httplib2.Http(
        timeout=90,
        proxy_info=httplib2.proxy_info_from_url(
            proxy, "https", env.get("NO_PROXY") or proxies.get("no")
        )
        if proxy
        else httplib2.proxy_info_from_environment,
    )
    # Google uses 308 without Location to acknowledge a resumable upload chunk.
    http.redirect_codes = http.redirect_codes - {308}
    return build(
        "androidpublisher", "v3", http=AuthorizedHttp(credentials, http=http), cache_discovery=False
    )


def upload_bundle(edits, params, bundle):
    from googleapiclient.http import MediaFileUpload

    media = MediaFileUpload(
        str(bundle), mimetype="application/octet-stream", chunksize=8 * 1024 * 1024, resumable=True
    )
    return edits.bundles().upload(**params, media_body=media).execute(num_retries=3)


def track_releases(edits, params):
    return {
        t["track"]: t.get("releases", [])
        for t in edits.tracks().list(**params).execute().get("tracks", [])
    }


def release_body(track, previous, status, name, code):
    if any(
        r["status"] in ("inProgress", "halted") or len(r.get("versionCodes", [])) > 1
        for r in previous
    ):
        raise ValueError(
            "This track contains a rollout or multi-code release; resolve its release policy in Play Console."
        )
    drafts = [r for r in previous if r["status"] == "draft"]
    if drafts and (status != "draft" or len(drafts) != 1):
        raise ValueError("Resolve the existing draft before replacing the completed release.")
    keep = [r for r in previous if r["status"] != "draft"] if status == "draft" else []
    new_release = dict(drafts[0]) if drafts else {}
    new_release.update(status=status, name=f"{name} ({code})", versionCodes=[str(code)])
    return {"track": track, "releases": [*keep, new_release]}


def release(project, action, output_format=None):
    project = project.resolve(strict=True)
    settings = read_settings(project / "settings/editor-android.toml")
    android = settings["android"]
    version = android["version"]
    play = settings.get("google_play")
    package = android["package"]
    if output_format is not None and (action != "build" or output_format not in FORMATS):
        raise ValueError("Select apk or aab for a local build.")
    formats = [output_format] if output_format else android["formats"]
    if action == "publish" and play is None:
        raise ValueError("Configure [google_play] to publish to the store.")
    state_dir = plain_path(project / "settings/private/android/.releases" / package)
    summary = {"package": package}
    profile = android.get("signing", {}).get("profile")
    if profile:
        summary["profile"] = profile
    if play is not None:
        summary.update(track=play["track"], releaseStatus=play["status"])
    state_file, pending = state_dir / "last-release.json", state_dir / "pending-release.json"
    with exclusive_build(SDK / "build/android-gradle/publish-locks" / (package + ".lock")):
        last = json.loads(state_file.read_text())["versionCode"] if state_file.exists() else 0
        base = max(version["code"], last)
        name, code = next_version(version, base)
        if action == "inspect":
            return {
                **summary,
                "nextLocalVersion": name,
                "versionCode": code,
                "formats": formats,
                "android": {
                    "artifactName": android["artifact_name"],
                    "orientation": android["orientation"],
                    "fullscreen": android["fullscreen"],
                    "minSdk": android["min_sdk"],
                    "targetSdk": android["target_sdk"],
                    "compileSdk": android["compile_sdk"],
                },
                "pendingCommit": pending.exists(),
            }
        env = toolchain()
        if action == "build":
            if pending.exists():
                raise ValueError(
                    "A prior commit is pending reconciliation. Use publish to reconcile the stored bundle."
                )
            artifacts = signed_packages(project, android, env, name, code, formats)
            return {**summary, "versionName": name, "versionCode": code, "artifacts": artifacts}
        state_dir.mkdir(mode=0o700, parents=True, exist_ok=True)
        track = play["track"]
        edits = google_publisher(private_file(project, play["service_account"]), env).edits()
        edit = edits.insert(packageName=package, body={}).execute()["id"]
        params = {"packageName": package, "editId": edit}
        committing = False
        try:
            tracks = track_releases(edits, params)
            versions = [
                int(v)
                for releases in tracks.values()
                for r in releases
                for v in r.get("versionCodes", [])
            ]
            bundles = {
                int(b["versionCode"]): b.get("sha256", "").lower()
                for b in edits.bundles().list(**params).execute().get("bundles", [])
            }
            versions.extend(bundles)
            versions.extend(
                int(a["versionCode"]) for a in edits.apks().list(**params).execute().get("apks", [])
            )
            if pending.exists():
                record = json.loads(pending.read_text())
                visible = any(
                    str(record["versionCode"]) in r.get("versionCodes", [])
                    and r["status"] == record["releaseStatus"]
                    for r in tracks.get(record["track"], [])
                )
                if visible and bundles.get(record["versionCode"]) == record["sha256"]:
                    atomic_json(state_file, record)
                    pending.unlink()
                    return {**record, "reconciled": True}
                if record["versionCode"] in versions:
                    raise ValueError(
                        "The pending version is present on Play with a different bundle, track or status."
                    )
                pending.unlink()
            name, code = next_version(version, max([base, *versions]))
            body = release_body(track, tracks.get(track, []), play["status"], name, code)
            artifacts = signed_packages(project, android, env, name, code, ["aab"])
            bundle = Path(artifacts["aab"])
            with bundle.open("rb") as stream:
                digest = hashlib.file_digest(stream, "sha256").hexdigest()
            upload = upload_bundle(edits, params, bundle)
            if int(upload["versionCode"]) != code or upload["sha256"].lower() != digest:
                raise ValueError("Google returned a different bundle version or digest.")
            edits.tracks().update(**params, track=track, body=body).execute()
            edits.validate(**params).execute()
            record = {
                **summary,
                "versionName": name,
                "versionCode": code,
                "sha256": digest,
                "artifacts": artifacts,
            }
            atomic_json(pending, record)
            committing = True
            edits.commit(**params).execute()
            atomic_json(state_file, record)
            pending.unlink()
            return {**record, "committed": True}
        finally:
            if not committing:
                try:
                    edits.delete(**params).execute()
                except Exception as error:
                    print(f"Play edit cleanup failed ({type(error).__name__}).", file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("inspect", "build", "publish"))
    parser.add_argument("--project", type=Path, required=True)
    parser.add_argument(
        "--format",
        choices=FORMATS,
        help="Build one format instead of the project's configured outputs",
    )
    args = parser.parse_args()
    try:
        result = release(args.project, args.action, args.format)
    except (KeyError, ValueError, OSError) as error:
        # JSON/TOML syntax errors report locations, never print credential values.
        print(json.dumps({"error": str(error)}))
        return 1
    except Exception as error:
        reason = getattr(getattr(error, "resp", None), "status", None)
        # Library exceptions and subprocess arguments may include private file names or HTTP data.
        print(
            json.dumps(
                {"error": f"Release operation failed ({type(error).__name__}, status {reason})."}
            )
        )
        return 1
    print(json.dumps(result))
    return 0


if __name__ == "__main__":
    sys.exit(main())
