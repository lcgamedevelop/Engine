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


def check_table(table, name, required, optional=()):
    if not isinstance(table, dict):
        raise ValueError(f"Expected the [{name}] settings table.")
    missing = required.difference(table)
    if missing:
        raise ValueError(f"Missing fields in [{name}]: " + ", ".join(sorted(missing)))
    if table.keys() - required - set(optional):
        raise ValueError(
            f"Unknown field in [{name}]. Expected: " + ", ".join(sorted(required.union(optional)))
        )


def version_layout(version):
    check_table(version, "android.version", {"name", "code", "weights", "increment_component"})
    weights = version["weights"]
    base, code, increment = (
        version["name"],
        version["code"],
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
    return (
        weights,
        code - sum(n * w for n, w in zip(digits, weights)),
        weights[increment - 1],
    )


def next_version(version, maximum):
    weights, offset, step = version_layout(version)
    highest = max(version["code"], maximum)
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
        try:
            settings = tomllib.load(stream)
        except tomllib.TOMLDecodeError as error:
            location = (
                f" at line {error.lineno}, column {error.colno}" if hasattr(error, "lineno") else ""
            )
            raise ValueError(f"Invalid Android project TOML{location}.") from None
    if "android" not in settings or settings.keys() - {"android", "google_play"}:
        raise ValueError("Use [android] with an optional [google_play] table.")
    android = settings["android"]
    check_table(
        android,
        "android",
        {
            "package",
            "label",
            "artifact_name",
            "orientation",
            "formats",
            "min_sdk",
            "target_sdk",
            "version",
        },
        {"fullscreen", "signing"},
    )
    if not isinstance(android["package"], str) or not re.fullmatch(
        r"[a-zA-Z][a-zA-Z0-9_]*(\.[a-zA-Z][a-zA-Z0-9_]*)+", android["package"]
    ):
        raise ValueError("Invalid Android application ID.")
    if (
        not isinstance(android["label"], str)
        or not android["label"].strip()
        or not isinstance(android["orientation"], str)
        or android["orientation"] not in ORIENTATIONS
    ):
        raise ValueError("Set the Android app label and screen orientation in project settings.")
    if not isinstance(android["artifact_name"], str) or not re.fullmatch(
        r"[\w-]+", android["artifact_name"], re.ASCII
    ):
        raise ValueError("The artifact name must contain letters, digits, underscores or hyphens.")
    if type(android.get("fullscreen", False)) is not bool:
        raise ValueError("The Android fullscreen setting must be a boolean.")
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
    version_layout(android["version"])
    if "signing" in android:
        signing = android["signing"]
        check_table(
            signing,
            "android.signing",
            {
                "keystore",
                "alias",
                "certificate_sha256",
                "store_password",
                "key_password",
            },
            {"profile"},
        )
        if not all(isinstance(value, str) for value in signing.values()):
            raise ValueError("Use strings for the fields in [android.signing].")
        if not signing["keystore"].strip() or not signing["alias"].strip():
            raise ValueError("Set the keystore path and alias in [android.signing].")
        if not re.fullmatch(r"[0-9a-fA-F]{64}", signing["certificate_sha256"]):
            raise ValueError("Invalid upload certificate SHA-256 in [android.signing].")
    if "google_play" in settings:
        play = settings["google_play"]
        check_table(play, "google_play", {"track", "status", "service_account"})
        if not isinstance(play["track"], str) or not re.fullmatch(
            r"[a-zA-Z0-9_.:-]+", play["track"]
        ):
            raise ValueError("Invalid Google Play track name.")
        if play["status"] not in ("draft", "completed"):
            raise ValueError("Expected a draft or completed Google Play release.")
        if not isinstance(play["service_account"], str) or not play["service_account"].strip():
            raise ValueError("Set the service account file path in [google_play].")
    return settings


def private_file(project, relative):
    relative = Path(relative)
    settings = project.resolve() / "settings"
    private = settings / "private"
    path = (settings / relative).resolve()
    if (
        relative.is_absolute()
        or private.resolve() != private
        or not path.is_relative_to(private)
        or not path.is_file()
    ):
        raise ValueError(
            "Select an existing file in this project's settings/private, relative to settings/."
        )
    return path
