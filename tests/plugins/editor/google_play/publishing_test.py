"""Verify Play version selection and transaction boundaries without contacting a store."""

import importlib.util
from contextlib import ExitStack
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import MagicMock, patch

ROOT = Path(__file__).resolve().parents[4]
spec = importlib.util.spec_from_file_location(
    "publisher", ROOT / "platforms/android/editor/google_play.py"
)
publisher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(publisher)


class PublishingTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.home = Path(self.temp.name)
        self.profile = {
            "package": "test.example",
            "track": "internal",
            "status": "completed",
            "last_version": "1.0.6.0",
        }
        self.edits = MagicMock()
        self.edits.insert.return_value.execute.return_value = {"id": "edit"}
        self.previous = {"status": "completed", "name": "old", "versionCodes": ["100007000"]}
        self.tracks = {"tracks": [{"track": "internal", "releases": [self.previous]}]}
        self.edits.tracks.return_value.list.return_value.execute.return_value = self.tracks
        self.edits.bundles.return_value.list.return_value.execute.return_value = {
            "bundles": [{"versionCode": 100008001}]
        }
        self.edits.apks.return_value.list.return_value.execute.return_value = {}
        stack = self.enterContext(ExitStack())

        def replace(name, **kwargs):
            return stack.enter_context(patch.object(publisher, name, **kwargs))

        replace("read_settings", return_value=({"profile": "sample"}, self.profile))
        replace("toolchain", return_value={})
        self.service = replace("google_publisher")
        self.service.return_value.edits.return_value = self.edits
        self.builder = replace("signed_bundle", return_value=self.home / "bundle.aab")
        (self.home / "bundle.aab").write_bytes(b"test signed bundle")
        self.digest = hashlib.sha256(b"test signed bundle").hexdigest()
        self.upload = replace(
            "upload_bundle", return_value={"versionCode": 100009000, "sha256": self.digest}
        )
        self.state = self.home / "state/test.example.json"
        self.pending = self.home / "state/test.example.pending.json"

    def run_release(self, mode):
        return publisher.release(self.home, self.home, mode)

    def test_publish_uses_versions_across_play_and_commits_once(self):
        result = self.run_release("publish")
        self.assertEqual(result["versionName"], "1.0.9.0")
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
        self.edits.bundles.return_value.list.return_value.execute.return_value = {"bundles": [entry]}
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
        track = publisher.release_body("internal", [self.previous, draft], "draft", "1.0.9.0", 100009000)
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
        (settings / "editor-google-play.toml").write_text("[google_play]")
        destination = original / "cache/google-play/test.example/android-project"
        publisher.android_project_snapshot(original, destination)
        self.assertEqual((destination / "assets/stage.scene").read_text(), "first")
        self.assertFalse((destination / "settings/editor-google-play.toml").exists())
        (destination / "cache/android-cache").write_text("retained")
        (assets / "stage.scene").unlink()
        (assets / "another.scene").write_text("second")
        publisher.android_project_snapshot(original, destination)
        self.assertFalse((destination / "assets/stage.scene").exists())
        self.assertEqual((destination / "cache/android-cache").read_text(), "retained")
        self.assertEqual((destination / "assets/another.scene").read_text(), "second")

    def test_version_rollover_and_nested_build_lock(self):
        self.assertEqual(publisher.next_version(999_999), ("0.1.0.0", 1_000_000))
        with self.assertRaises(ValueError):
            publisher.next_version(publisher.MAX_CODE)
        with publisher.exclusive_build(self.home / "state.lock"):
            with self.assertRaises(ValueError):
                with publisher.exclusive_build(self.home / "state.lock"):
                    pass


if __name__ == "__main__":
    unittest.main()
