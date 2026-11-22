"""Read one project's Android packaging and release settings."""

from pathlib import Path
import re
import tomllib

MAX_CODE = 2_100_000_000
FORMATS = ("apk", "aab")
ORIENTATIONS = {
    "unspecified": -1,
    "landscape": 0,
    "portrait": 1,
    "user": 2,
    "behind": 3,
    "sensor": 4,
    "nosensor": 5,
    "sensorLandscape": 6,
    "sensorPortrait": 7,
    "reverseLandscape": 8,
    "reversePortrait": 9,
    "fullSensor": 10,
    "userLandscape": 11,
    "userPortrait": 12,
    "fullUser": 13,
    "locked": 14,
}


def version_layout(version):
    weights = version["version_weights"]
    base, code, increment = (
        version["version_name"],
        version["version_code"],
        version["increment_component"],
    )
    if (
        not isinstance(weights, list)
        or not weights
        or any(type(n) is not int or n <= 0 for n in weights)
        or weights[-1] != 1
        or any(a <= b or a % b for a, b in zip(weights, weights[1:]))
        or type(increment) is not int
        or not 1 <= increment <= len(weights)
        or type(code) is not int
        or not 0 <= code <= MAX_CODE
    ):
        raise ValueError("Invalid Android version weights, base code or increment component.")
    if not isinstance(base, str):
        raise ValueError("Use a quoted Android version name in project settings.")
    parts = base.split(".")
    if len(parts) != len(weights) or not all(re.fullmatch(r"0|[1-9][0-9]*", p) for p in parts):
        raise ValueError("Version name must match the project's numeric version components.")
    digits = list(map(int, parts))
    if any(n >= high // low for n, high, low in zip(digits[1:], weights, weights[1:])):
        raise ValueError("A version component exceeds its configured range.")
    return weights, code - sum(n * w for n, w in zip(digits, weights)), weights[increment - 1]


def next_version(version, maximum):
    weights, offset, step = version_layout(version)
    highest = max(version["version_code"], maximum)
    value = ((highest - offset) // step + 1) * step
    code = value + offset
    if not 0 < code <= MAX_CODE:
        raise ValueError("The Android version line is exhausted.")
    parts = []
    for weight in weights:
        digit, value = divmod(value, weight)
        parts.append(str(digit))
    return ".".join(parts), code


def read_settings(path):
    with path.open("rb") as stream:
        settings = tomllib.load(stream)
    if set(settings) != {"android"}:
        raise ValueError("Place all package settings in the [android] table.")
    android = settings["android"]
    required = {
        "package",
        "label",
        "artifact_name",
        "orientation",
        "formats",
        "min_sdk",
        "target_sdk",
        "version_name",
        "version_code",
        "version_weights",
        "increment_component",
    }
    missing = required.difference(android)
    if missing:
        raise ValueError("Missing project Android settings: " + ", ".join(sorted(missing)))
    if not re.fullmatch(r"[a-zA-Z][a-zA-Z0-9_]*(\.[a-zA-Z][a-zA-Z0-9_]*)+", android["package"]):
        raise ValueError("Invalid Android application ID.")
    if not android["label"].strip() or android["orientation"] not in ORIENTATIONS:
        raise ValueError("Set the Android app label and screen orientation in project settings.")
    if not re.fullmatch(r"[\w-]+", android["artifact_name"], re.ASCII):
        raise ValueError("The artifact name must contain letters, digits, underscores or hyphens.")
    formats = android["formats"]
    if (
        not isinstance(formats, list)
        or not formats
        or any(f not in FORMATS for f in formats)
        or len(formats) != len(set(formats))
    ):
        raise ValueError("Project outputs must be a list of distinct APK or AAB formats.")
    minimum, target = android["min_sdk"], android["target_sdk"]
    if type(minimum) is not int or type(target) is not int or not 28 <= minimum <= target:
        raise ValueError("Android requires target SDK >= minimum SDK >= 28.")
    version_layout(android)
    play_keys = {"play_track", "play_status", "service_account"}
    if play_keys.intersection(android):
        if not play_keys.issubset(android):
            raise ValueError(
                "Set play_track, play_status and service_account together in project settings."
            )
        if not re.fullmatch(r"[a-zA-Z0-9_.:-]+", android["play_track"]):
            raise ValueError("Invalid Google Play track name.")
        if android["play_status"] not in ("draft", "completed"):
            raise ValueError("Expected a draft or completed Google Play release.")
    return android


def private_file(project, profile, relative):
    if not isinstance(profile, str) or not re.fullmatch(r"[a-zA-Z0-9][a-zA-Z0-9_-]{0,79}", profile):
        raise ValueError("Invalid Android signing profile name in project settings.")
    relative = Path(relative)
    base = project.resolve() / "settings/private/android" / profile
    path = (base / relative).resolve(strict=True)
    if (
        relative.is_absolute()
        or base.resolve() != base
        or not path.is_relative_to(base)
        or not path.is_file()
    ):
        raise ValueError(
            "Select a file inside the current project's private Android signing profile."
        )
    return path
