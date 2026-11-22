# /// script
# requires-python = ">=3.11"
# dependencies = ["google-api-python-client==2.197.0", "google-auth==2.53.0", "PySocks==1.7.1"]
# ///
"""Build or publish the open Engine project's signed Android App Bundle."""

import argparse
import base64
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

SDK = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(SDK / "scripts"))
from build_lock import exclusive_build
from artifact_output import staged_output
from prepare_android_assets import asset_files

MAX_CODE = 2_100_000_000
ANDROID = "{http://schemas.android.com/apk/res/android}"


def version_code(name):
    if not isinstance(name, str) or not re.fullmatch(r"(0|[1-9][0-9]*)(\.(0|[1-9][0-9]*)){3}", name):
        raise ValueError("Expected a four-part Android version.")
    major, minor, patch, revision = map(int, name.split("."))
    code = major * 100_000_000 + minor * 1_000_000 + patch * 1000 + revision
    if minor > 99 or patch > 999 or revision or not 0 <= code <= MAX_CODE:
        raise ValueError("Invalid Android release version; the fourth field must be zero.")
    return code


def next_version(maximum):
    code = (maximum // 1000 + 1) * 1000
    name = f"{code // 100_000_000}.{code // 1_000_000 % 100}.{code // 1000 % 1000}.0"
    if version_code(name) != code:
        raise ValueError("The Android version line is exhausted.")
    return name, code


def atomic_json(path, data):
    with tempfile.NamedTemporaryFile(mode="w", dir=path.parent, encoding="utf-8", delete=False) as f:
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


def read_settings(project, home):
    settings = tomllib.loads((project / "settings/editor-google-play.toml").read_text())["google_play"]
    profile = settings["profile"]
    if not re.fullmatch(r"[a-zA-Z0-9][a-zA-Z0-9_-]{0,79}", profile):
        raise ValueError("Invalid publishing profile name.")
    folder = home / profile
    data = json.loads((folder / "profile.json").read_text())
    if not re.fullmatch(r"[a-zA-Z][a-zA-Z0-9_]*(\.[a-zA-Z][a-zA-Z0-9_]*)+", data["package"]):
        raise ValueError("Invalid Android application ID.")
    if not re.fullmatch(r"[\w.-]+", data["track"]) or data["status"] not in ("draft", "completed"):
        raise ValueError("Expected a Play track and a draft or completed release.")
    version_code(data["last_version"])
    if not re.fullmatch(r"[a-fA-F0-9]{64}", data["certificate_sha256"]):
        raise ValueError("Invalid upload certificate fingerprint.")
    for key in ("keystore", "service_account"):
        data[key] = (folder / data[key]).resolve(strict=True)
    return settings, data


def toolchain(home):
    config = home / "toolchain.toml"
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
            raise ValueError("Runtime or import settings changed during the Android build snapshot.")
    return destination


def signed_bundle(project, settings, profile, env, name, code):
    package = profile["package"]
    output = SDK / "output/android/arm64/release/runtime" / package
    output.mkdir(parents=True, exist_ok=True)
    logs = project / "cache/google-play"
    logs.mkdir(parents=True, exist_ok=True)
    logfile = logs / f"build-{code}.log"
    jdk = Path(env["JAVA_HOME"]) / "bin"
    suffix = ".exe" if os.name == "nt" else ""
    keytool = str(jdk / ("keytool" + suffix))
    jarsigner = str(jdk / ("jarsigner" + suffix))
    secret_env = {
        **env,
        "ENGINE_UPLOAD_STORE_PASSWORD": profile["store_password"],
        "ENGINE_UPLOAD_KEY_PASSWORD": profile["key_password"],
    }
    store = ["-keystore", str(profile["keystore"]), "-storepass:env", "ENGINE_UPLOAD_STORE_PASSWORD"]
    fingerprint = profile["certificate_sha256"].lower()
    cert = subprocess.run(
        [keytool, "-exportcert", *store, "-alias", profile["alias"]],
        env=secret_env,
        capture_output=True,
        check=True,
    ).stdout
    if hashlib.sha256(cert).hexdigest() != fingerprint:
        raise ValueError("The keystore certificate does not match this app's upload certificate.")
    android_project = android_project_snapshot(project, logs / package / "android-project")
    with tempfile.TemporaryDirectory(dir=output) as tmp, logfile.open("w") as log:
        stage = Path(tmp)
        unsigned, signed = stage / "unsigned.aab", stage / "signed.aab"

        def run(args, *, signing=False):
            if subprocess.run(
                args, env=secret_env if signing else env, stdout=log, stderr=subprocess.STDOUT
            ).returncode:
                raise ValueError(f"Android bundle step failed ({Path(args[0]).name}); see {logfile}")

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
                "--application-id",
                package,
                "--version-name",
                name,
                "--version-code",
                str(code),
                "--target-sdk",
                str(profile["target_sdk"]),
                "--label",
                settings.get("label", project.name),
                "--orientation",
                settings.get("orientation", "unspecified"),
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
                profile["alias"],
            ],
            signing=True,
        )
        run([jarsigner, "-verify", str(signed)])
        certs = subprocess.check_output(
            [keytool, "-printcert", "-rfc", "-jarfile", str(signed)], env=env
        )
        match = re.search(rb"-----BEGIN CERTIFICATE-----(.*?)-----END CERTIFICATE-----", certs, re.S)
        if not match or hashlib.sha256(base64.b64decode(match[1])).hexdigest() != fingerprint:
            raise ValueError("The signed bundle does not have the configured upload certificate.")
        java = str(jdk / ("java" + suffix))
        bundletool = [java, "-jar", env["ENGINE_BUNDLETOOL"]]
        run(bundletool + ["validate", "--bundle", str(signed)])
        xml = subprocess.check_output(
            bundletool + ["dump", "manifest", "--bundle", str(signed)], env=env
        )
        manifest = ET.fromstring(xml)
        target = manifest.find("uses-sdk")
        if (
            (
                manifest.get("package"),
                manifest.get(ANDROID + "versionName"),
                manifest.get(ANDROID + "versionCode"),
            )
            != (package, name, str(code))
            or target is None
            or target.get(ANDROID + "targetSdkVersion") != str(profile["target_sdk"])
        ):
            raise ValueError("The signed bundle's manifest does not match the release profile.")
        destination = output / f"{package}-{code}.aab"
        os.replace(signed, destination)
    return destination


def google_publisher(profile, env):
    import httplib2
    from google.oauth2 import service_account
    from google_auth_httplib2 import AuthorizedHttp
    from googleapiclient.discovery import build

    account = json.loads(profile["service_account"].read_text())
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
        proxy_info=httplib2.proxy_info_from_url(proxy, "https", env.get("NO_PROXY") or proxies.get("no"))
        if proxy
        else httplib2.proxy_info_from_environment,
    )
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
        r["status"] in ("inProgress", "halted") or len(r.get("versionCodes", [])) > 1 for r in previous
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


def release(project, home, action):
    settings, profile = read_settings(project, home)
    package, track = profile["package"], profile["track"]
    state_dir = home / "state"
    state_dir.mkdir(mode=0o700, exist_ok=True)
    state_file, pending = state_dir / (package + ".json"), state_dir / (package + ".pending.json")
    summary = {
        "profile": settings["profile"],
        "package": package,
        "track": track,
        "releaseStatus": profile["status"],
    }
    with exclusive_build(state_dir / (package + ".lock")):
        last = json.loads(state_file.read_text())["versionCode"] if state_file.exists() else 0
        base = max(version_code(profile["last_version"]), last)
        name, code = next_version(base)
        if action == "inspect":
            return {
                **summary,
                "nextLocalVersion": name,
                "versionCode": code,
                "pendingCommit": pending.exists(),
            }
        env = toolchain(home)
        if action == "build":
            if pending.exists():
                raise ValueError(
                    "A prior commit is pending reconciliation. Use publish to reconcile the stored bundle."
                )
            bundle = signed_bundle(project, settings, profile, env, name, code)
            return {**summary, "versionName": name, "versionCode": code, "bundle": str(bundle)}
        edits = google_publisher(profile, env).edits()
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
            name, code = next_version(max([base, *versions]))
            body = release_body(track, tracks.get(track, []), profile["status"], name, code)
            bundle = signed_bundle(project, settings, profile, env, name, code)
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
                "bundle": str(bundle),
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
    args = parser.parse_args()
    home = Path(
        os.environ.get("ENGINE_GOOGLE_PLAY_HOME", str(Path.home() / ".config/engine/google-play"))
    )
    try:
        result = release(args.project.resolve(strict=True), home, args.action)
    except (KeyError, ValueError, OSError) as error:
        # JSON/TOML syntax errors report locations, never print credential values.
        print(json.dumps({"error": str(error)}))
        return 1
    except Exception as error:
        reason = getattr(getattr(error, "resp", None), "status", None)
        # Library exceptions and subprocess arguments may include private file names or HTTP data.
        print(
            json.dumps({"error": f"Release operation failed ({type(error).__name__}, status {reason})."})
        )
        return 1
    print(json.dumps(result))
    return 0


if __name__ == "__main__":
    sys.exit(main())
