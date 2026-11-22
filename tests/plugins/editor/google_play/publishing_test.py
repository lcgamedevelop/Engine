"""Verify Play version selection and transaction boundaries without contacting a store."""

from contextlib import ExitStack
import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import MagicMock, patch

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / "platforms/android/editor"))
import google_play as publisher
from project_package import MAX_CODE, next_version, private_file, read_settings


class PublishingTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.home = Path(self.temp.name)
        private = self.home / "settings/private"
        self.identity = private / "android/test"
        self.identity.mkdir(parents=True)
        (self.identity / "account.json").write_text("{}")
        self.config = private.parent / "editor-android.toml"
        fingerprint = "00" * 32
        self.config.write_text(f"""[android]
package = "test.example"
label = "Example"
artifact_name = "example"
orientation = "landscape"
formats = ["apk", "aab"]
min_sdk = 28
target_sdk = 35

[android.version]
name = "1.0.6.0"
code = 100006000
weights = [100000000, 1000000, 1000, 1]
increment_component = 3

[android.signing]
profile = "release-team"
keystore = "private/android/test/upload.jks"
alias = "upload"
certificate_sha256 = "{fingerprint}"
store_password = "fixture-store"
key_password = "fixture-key"

[google_play]
track = "internal"
status = "completed"
service_account = "private/android/test/account.json"
""")
        self.edits = MagicMock()
        self.edits.insert.return_value.execute.return_value = {"id": "edit"}
        self.previous = {
            "status": "completed",
            "name": "old",
            "versionCodes": ["100007000"],
        }
        self.tracks = {"tracks": [{"track": "internal", "releases": [self.previous]}]}
        self.edits.tracks.return_value.list.return_value.execute.return_value = self.tracks
        self.edits.bundles.return_value.list.return_value.execute.return_value = {
            "bundles": [{"versionCode": 100008001}]
        }
        self.edits.apks.return_value.list.return_value.execute.return_value = {}
        stack = self.enterContext(ExitStack())

        def replace(name, **kwargs):
            return stack.enter_context(patch.object(publisher, name, **kwargs))

        stack.enter_context(patch.object(publisher, "SDK", self.home / "sdk"))
        replace("toolchain", return_value={})
        self.service = replace("google_publisher")
        self.service.return_value.edits.return_value = self.edits
        self.builder = replace(
            "signed_packages", return_value={"aab": str(self.home / "bundle.aab")}
        )
        (self.home / "bundle.aab").write_bytes(b"test signed bundle")
        self.digest = hashlib.sha256(b"test signed bundle").hexdigest()
        self.upload = replace(
            "upload_bundle",
            return_value={"versionCode": 100009000, "sha256": self.digest},
        )
        self.state = private / "android/.releases/test.example/last-release.json"
        self.pending = self.state.with_name("pending-release.json")

    def run_release(self, mode):
        return publisher.release(self.home, mode)

    def test_publish_uses_versions_across_play_and_commits_once(self):
        result = self.run_release("publish")
        self.assertEqual(result["versionName"], "1.0.9.0")
        self.assertEqual(result["profile"], "release-team")
        self.service.assert_called_once_with((self.identity / "account.json").resolve(), {})
        self.assertEqual(json.loads(self.state.read_text())["versionCode"], 100009000)
        self.assertFalse(self.pending.exists())
        self.edits.validate.return_value.execute.assert_called_once()
        self.edits.commit.return_value.execute.assert_called_once()
        self.edits.delete.assert_not_called()
        body = self.edits.tracks.return_value.update.call_args.kwargs["body"]
        self.assertEqual(body["track"], "internal")
        self.assertEqual(body["releases"][0]["versionCodes"], ["100009000"])

    def test_failed_validation_discards_edit_without_recording_version(self):
        self.edits.validate.return_value.execute.side_effect = RuntimeError("validation")
        with self.assertRaises(RuntimeError):
            self.run_release("publish")
        self.edits.delete.assert_called_once()
        self.edits.commit.assert_not_called()
        self.assertFalse(self.state.exists())
        self.assertFalse(self.pending.exists())

    def test_uncertain_commit_reconciles_without_second_upload(self):
        self.edits.commit.return_value.execute.side_effect = TimeoutError()
        with self.assertRaises(TimeoutError):
            self.run_release("publish")
        self.assertTrue(self.pending.exists())
        self.assertFalse(self.state.exists())
        self.previous["versionCodes"] = ["100009000"]
        entry = {"versionCode": 100009000, "sha256": "0" * 64}
        self.edits.bundles.return_value.list.return_value.execute.return_value = {
            "bundles": [entry]
        }
        with self.assertRaises(ValueError):
            self.run_release("build")
        with self.assertRaises(ValueError):
            self.run_release("publish")
        entry["sha256"] = self.digest
        receipt = self.run_release("publish")
        self.assertTrue(receipt["reconciled"])
        self.upload.assert_called_once()
        self.assertFalse(self.pending.exists())

    def test_busy_track_is_preserved_and_local_build_stays_offline(self):
        self.previous["status"] = "inProgress"
        with self.assertRaises(ValueError):
            self.run_release("publish")
        self.edits.tracks.return_value.update.assert_not_called()
        self.service.reset_mock()
        receipt = self.run_release("build")
        self.assertEqual(receipt["versionName"], "1.0.7.0")
        self.service.assert_not_called()
        self.assertFalse(self.state.exists())

    def test_replacing_draft_preserves_the_completed_release_and_draft_notes(self):
        draft = {
            "status": "draft",
            "versionCodes": ["100008000"],
            "releaseNotes": [{"language": "en-US", "text": "Test draft"}],
        }
        track = publisher.release_body(
            "internal", [self.previous, draft], "draft", "1.0.9.0", 100009000
        )
        self.assertEqual(track["releases"][0], self.previous)
        self.assertEqual(track["releases"][1]["versionCodes"], ["100009000"])
        self.assertEqual(track["releases"][1]["releaseNotes"], draft["releaseNotes"])
        self.assertEqual(draft["versionCodes"], ["100008000"])

    def test_android_workspace_isolated_from_open_editor_and_publishing_secrets(self):
        original = self.home / "game"
        assets, settings = original / "assets", original / "settings"
        assets.mkdir(parents=True)
        settings.mkdir()
        (assets / "stage.scene").write_text("first")
        (settings / "runtime-game.toml").write_text("[common]")
        (settings / "editor-android.toml").write_text("[android]")
        (settings / "private").mkdir()
        (settings / "private/upload.jks").write_bytes(b"signing fixture")
        destination = original / "cache/google-play/test.example/android-project"
        publisher.android_project_snapshot(original, destination)
        self.assertEqual((destination / "assets/stage.scene").read_text(), "first")
        self.assertFalse((destination / "settings/editor-android.toml").exists())
        self.assertFalse((destination / "settings/private").exists())
        (destination / "cache/android-cache").write_text("retained")
        (assets / "stage.scene").unlink()
        (assets / "another.scene").write_text("second")
        publisher.android_project_snapshot(original, destination)
        self.assertFalse((destination / "assets/stage.scene").exists())
        self.assertEqual((destination / "cache/android-cache").read_text(), "retained")
        self.assertEqual((destination / "assets/another.scene").read_text(), "second")

    def test_version_rollover_and_nested_build_lock(self):
        version = dict(read_settings(self.config)["android"]["version"], name="0.0.0.0", code=0)
        self.assertEqual(next_version(version, 999_999), ("0.1.0.0", 1_000_000))
        with self.assertRaises(ValueError):
            next_version(version, MAX_CODE)
        with publisher.exclusive_build(self.home / "state.lock"):
            with self.assertRaises(ValueError):
                with publisher.exclusive_build(self.home / "state.lock"):
                    pass

    def test_build_uses_project_formats_without_a_store_account(self):
        self.config.write_text(self.config.read_text().partition("[google_play]")[0])
        (self.identity / "account.json").unlink()
        self.run_release("build")
        self.assertEqual(self.builder.call_args.args[-1], ["apk", "aab"])
        for fmt in ("apk", "aab"):
            publisher.release(self.home, "build", fmt)
            self.assertEqual(self.builder.call_args.args[-1], [fmt])
        self.service.assert_not_called()
        with self.assertRaises(ValueError):
            self.run_release("publish")
        self.assertFalse(self.pending.exists())

    def test_project_version_schemes_and_private_files_are_independent(self):
        android = read_settings(ROOT / "projects/showcase/settings/editor-android.toml")["android"]
        self.assertEqual(next_version(android["version"], 5), ("0.1.0", 6))
        settings = self.home / "settings/editor-android.toml"
        template = (ROOT / "projects/showcase/settings/editor-android.toml").read_text()
        self.assertTrue(android["fullscreen"])
        settings.write_text(template.replace("[android]\n", '[android]\nfullscreen = "true"\n'))
        with self.assertRaises(ValueError):
            read_settings(settings)
        second = {
            "name": "3",
            "code": 126,
            "weights": [1],
            "increment_component": 1,
        }
        self.assertEqual(next_version(second, 130), ("8", 131))
        with self.assertRaises(ValueError):
            next_version(dict(second, weights=[2, 1]), 130)
        outside = self.home / "other-project.key"
        outside.write_text("fixture")
        with self.assertRaises(ValueError):
            private_file(self.home, outside)
        with self.assertRaises(ValueError):
            private_file(self.home, "private/../../other-project.key")
        try:
            (self.identity / "linked.key").symlink_to(outside)
        except OSError:
            return  # Windows hosts may restrict unprivileged symbolic links.
        with self.assertRaises(ValueError):
            private_file(self.home, "private/android/test/linked.key")

    def test_new_game_inherits_engine_defaults_and_can_override_product_choices(self):
        project = """[android]
package = "test.example"
label = "Crystal Vault"
[android.version]
name = "1.0-beta"
code = 12
"""
        self.config.write_text(project)
        receipt = self.run_release("inspect")
        self.assertEqual((receipt["nextLocalVersion"], receipt["versionCode"]), ("1.0-beta", 13))
        self.assertEqual(receipt["formats"], ["apk", "aab"])
        self.assertEqual(
            receipt["android"],
            dict(
                artifactName="crystal-vault",
                orientation="unspecified",
                fullscreen=True,
                minSdk=28,
                targetSdk=36,
                compileSdk=36,
            ),
        )
        self.config.write_text(
            project.replace(
                "[android]\n",
                """[android]
orientation = "portrait"
fullscreen = false
min_sdk = 29
target_sdk = 34
formats = ["apk"]
""",
            )
        )
        receipt = self.run_release("inspect")
        self.assertEqual(receipt["formats"], ["apk"])
        self.assertEqual(
            receipt["android"],
            dict(
                artifactName="crystal-vault",
                orientation="portrait",
                fullscreen=False,
                minSdk=29,
                targetSdk=34,
                compileSdk=36,
            ),
        )
        self.config.write_text(project.replace("[android]\n", "[android]\nmin_sdk = 37\n"))
        android = read_settings(self.config)["android"]
        self.assertEqual(
            (android["min_sdk"], android["target_sdk"], android["compile_sdk"]), (37, 37, 37)
        )
        self.config.write_text(project.replace("[android]\n", "[android]\ncompile_sdk = 34\n"))
        with self.assertRaises(ValueError):
            read_settings(self.config)
        with self.assertRaises(ValueError):
            next_version({"name": "1.0", "code": MAX_CODE}, 0)
        for partial in ({"weights": [1]}, {"increment_component": 1}):
            with self.subTest(partial=partial), self.assertRaises(ValueError):
                next_version({"name": "1", "code": 0, **partial}, 0)

    def test_inline_signing_values_and_group_boundaries(self):
        text = self.config.read_text()
        signing = read_settings(self.config)["android"]["signing"]
        self.assertEqual(signing["store_password"], "fixture-store")
        self.assertEqual(signing["key_password"], "fixture-key")
        receipt = json.dumps(self.run_release("inspect"))
        for password in ("fixture-store", "fixture-key"):
            self.assertNotIn(password, receipt)
        without_pin = "\n".join(
            line for line in text.splitlines() if not line.startswith("certificate_sha256 =")
        )
        self.config.write_text(without_pin)
        self.assertNotIn("certificate_sha256", read_settings(self.config)["android"]["signing"])
        for old, wrong in (
            ("00" * 32, "not-a-fingerprint"),
            ("[google_play]", "[android.google_play]"),
            ("store_password =", "password_file ="),
            ("[android]", '[android]\ntrack = "internal"'),
            ("[android.version]", "[version]"),
        ):
            with self.subTest(wrong=wrong), self.assertRaises(ValueError):
                self.config.write_text(text.replace(old, wrong))
                read_settings(self.config)

    def test_bundletool_password_files_stay_in_private_cache_and_are_removed_on_failure(self):
        cache = self.home / "cache/google-play"
        cache.mkdir(parents=True)
        signing = read_settings(self.config)["android"]["signing"]
        with self.assertRaisesRegex(RuntimeError, "signer failed"):
            with publisher.bundletool_password_args(self.home, signing) as args:
                for field, option, arg in zip(
                    ("store_password", "key_password"),
                    ("--ks-pass", "--key-pass"),
                    args,
                    strict=True,
                ):
                    prefix, path = arg.split("=file:", 1)
                    self.assertEqual(prefix, option)
                    self.assertNotIn(signing[field], arg)
                    path = Path(path)
                    self.assertTrue(path.is_relative_to(cache))
                    self.assertEqual(path.read_text(encoding="utf-8"), signing[field])
                    if os.name != "nt":
                        self.assertEqual(path.stat().st_mode & 0o077, 0)
                raise RuntimeError("signer failed")
        self.assertEqual(list(cache.iterdir()), [])


if __name__ == "__main__":
    unittest.main()
