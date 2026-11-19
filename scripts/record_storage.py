"""Select one record storage root per run, preferring the configured external volume."""

import json
import os
from pathlib import Path

_selected_root = None
_primary_root = None


def record_root():
    global _selected_root, _primary_root
    if _selected_root is not None:
        if not _selected_root.is_dir():
            raise RuntimeError("The record storage selected for this run became unavailable")
        return _selected_root
    repository = Path(__file__).resolve().parents[1]
    config_path = repository / ".cache/record-storage.json"
    config = json.loads(config_path.read_text()) if config_path.is_file() else {}
    configured = os.environ.get("ENGINE_RECORDS_ROOT") or config.get("root")
    if not configured:
        raise RuntimeError("Configure ENGINE_RECORDS_ROOT or .cache/record-storage.json first")
    primary = Path(configured)
    fallback_value = os.environ.get("ENGINE_RECORDS_FALLBACK_ROOT") or config.get("fallbackRoot")
    fallback = Path(fallback_value) if fallback_value else None
    mount = config.get("requiredMount")
    available = primary.is_dir() and (primary / ".engine-records-root").is_file()
    if mount and primary != fallback:
        available = available and Path(mount).is_mount() and primary.is_relative_to(Path(mount))
    root = primary if available else fallback
    if root is None or not root.is_absolute():
        raise RuntimeError("No absolute record storage root is available")
    for part in (root, *root.parents):
        if part.is_symlink():
            raise RuntimeError("The record storage root must not contain symbolic links")
    if not available:
        root.mkdir(parents=True, exist_ok=True)
        (root / ".engine-records-root").write_text("engine-records-v1\n")
    scratch = root / "scratch"
    scratch.mkdir(exist_ok=True)
    os.environ["ENGINE_RECORDS_ROOT"] = str(root)
    os.environ["ENGINE_CAPTURE_ROOT"] = str(root)
    if fallback is not None:
        os.environ["ENGINE_RECORDS_FALLBACK_ROOT"] = str(fallback)
        os.environ["ENGINE_CAPTURE_FALLBACK_ROOT"] = str(fallback)
    os.environ["TMPDIR"] = str(scratch) + os.sep
    _primary_root, _selected_root = primary, root
    return root


def record_path(*parts):
    root = record_root()
    path = root.joinpath(*parts)
    if not path.is_relative_to(root) or ".." in path.parts:
        raise ValueError("Record paths must remain inside the selected storage root")
    return path


def record_output(path):
    root = record_root()
    path = Path(path).resolve()
    if not path.is_relative_to(root):
        if root != _primary_root and path.is_relative_to(_primary_root):
            path = root / path.relative_to(_primary_root)
        else:
            raise ValueError("Record output must remain under the selected storage root")
    return path


if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", nargs=argparse.REMAINDER,
                        help="Program and arguments to run with the selected record storage")
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command:
        parser.error("Provide a program to run after --")
    record_root()
    artifacts = record_path("records/validation/ctest/window-render")
    artifacts.mkdir(parents=True, exist_ok=True)
    os.environ["ENGINE_RENDER_TEST_ARTIFACTS"] = str(artifacts)
    try:
        os.execvpe(command[0], command, os.environ)
    except OSError as error:
        parser.exit(1, f"Cannot start the requested program: {error}\n")
