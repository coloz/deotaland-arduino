"""Verify an Arduino library ZIP against the source checkout (stdlib only)."""
import argparse
import hashlib
import posixpath
import re
import zipfile
from pathlib import Path
from urllib.parse import unquote, urlsplit


ROOT = "deotaland-arduino/"
ROOT_FILES = ("library.properties", "keywords.txt", "LICENSE", "README.md")
SOURCE_DIRS = ("src", "examples", "docs")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def generated(name):
    return bool(
        re.search(r"(?:^|/)(?:build|\.build(?:[-_][^/]*)?|managed_components|__pycache__)(?:/|$)", name)
        or re.search(r"\.(?:bin|elf|map|o|obj|d|pyc|log|tmp|zip)$", name, re.IGNORECASE)
        or posixpath.basename(name) in ("sdkconfig", "sdkconfig.old", "dependencies.lock")
    )


def source_files(library):
    files = {name: (library / name).read_bytes() for name in ROOT_FILES}
    for directory in SOURCE_DIRS:
        for path in (library / directory).rglob("*"):
            if path.is_file():
                name = path.relative_to(library).as_posix()
                if not generated(name) and not any(part.startswith(".") for part in Path(name).parts):
                    files[name] = path.read_bytes()
    return files


def check(package, library):
    with zipfile.ZipFile(package) as archive:
        entries = [entry for entry in archive.infolist() if not entry.is_dir()]
        names = [entry.filename.replace("\\", "/") for entry in entries]
        require(len(names) == len(set(names)), "Duplicate ZIP entries")
        require(len(names) == len({name.casefold() for name in names}), "Case-colliding ZIP entries")
        files = {name: archive.read(entry) for name, entry in zip(names, entries)}
    require(all(name.startswith(ROOT) for name in files), "Unexpected ZIP root")
    require(all(name == posixpath.normpath(name) and ".." not in name.split("/") for name in files), "Unsafe ZIP path")
    relative = {name[len(ROOT):]: content for name, content in files.items()}
    require(all(name in ROOT_FILES or name.split("/", 1)[0] in SOURCE_DIRS for name in relative), "Unexpected directory or file in ZIP")
    require(not any(generated(name) for name in relative), "Generated products in ZIP")
    required = (
        "src/Deotaland.h", "src/DeotalandImageUpload.cpp",
        "src/vendor/nimble-arduino/DeotalandNimbleConfig.h",
        "src/vendor/nimble-arduino/LICENSE", "src/vendor/nimble-arduino/NOTICE",
        "src/vendor/nimble-arduino/nimble/LICENSE", "src/vendor/nimble-arduino/nimble/NOTICE",
        "examples/CameraSnapshot/CameraCapture.cpp", "examples/CameraSnapshot/camera_pins.h",
        "examples/StoredJpegUpload/sample_jpeg.h", "examples/ImageSequence/ImageSequence.ino",
        "examples/CoCDiagnostics/CoCDiagnostics.ino", "examples/CoCOtaUpdate/CoCOtaUpdate.ino",
        "examples/AiotBasicAgent/AiotBasicAgent.ino", "examples/AiotBasicAgent/BoardRecording.cpp",
        "examples/BasicBLE/BasicBLE.ino", "examples/VoicePTT/VoicePTT.ino",
        "examples/RealtimeASR/RealtimeASR.ino", "examples/BufferedPlayback/BufferedPlayback.ino",
    )
    require(all(name in relative for name in ROOT_FILES + required), "Missing delivery files")
    require(not any(re.match(r"src/(?:wifi_|cloud_|transport_wifi\.|portable_backends\.|worker_wake\.)",
                             name, re.IGNORECASE) for name in relative), "Removed network backend in ZIP")
    require(not any(re.match(r"examples/(?:BasicWiFi|WiFiProvisioning|CloudActivation|WiFiMqttControl)/",
                             name, re.IGNORECASE) for name in relative), "Removed network example in ZIP")
    expected = source_files(library)
    require(set(relative) == set(expected), "File list differs from checkout: " + repr(sorted(set(relative) ^ set(expected))))
    for name, content in expected.items():
        require(hashlib.sha256(relative[name]).digest() == hashlib.sha256(content).digest(), "Content differs from checkout: " + name)
    properties = relative["library.properties"].decode("utf-8-sig")
    match = re.search(r"^version=(.+)$", properties, re.MULTILINE)
    require(match is not None, "Missing library version")
    version = match.group(1).strip()
    link_count = include_count = 0
    for name, content in relative.items():
        if name.endswith(".md"):
            for target in re.findall(r"\[[^\]]+\]\(([^)]+)\)", content.decode("utf-8-sig")):
                target = target.strip().strip("<>")
                if target.startswith("#") or urlsplit(target).scheme:
                    continue
                target = unquote(target.split("#", 1)[0])
                if target:
                    resolved = posixpath.normpath(posixpath.join(posixpath.dirname(name), target))
                    require(resolved in relative, "Broken link: " + name + " -> " + target)
                    link_count += 1
        if name.endswith((".c", ".cpp", ".h")):
            for target in re.findall(
                r'^\s*#\s*include\s*["<](vendor/nimble-arduino/[^">]+)',
                content.decode("utf-8-sig"), re.MULTILINE,
            ):
                require("src/" + target in relative, "Missing/case-mismatched include: " + target)
                include_count += 1
    sketches = [name for name in relative if name.startswith("examples/") and name.endswith(".ino")]
    for sketch in sketches:
        readme = posixpath.join(posixpath.dirname(sketch), "README.md")
        require(readme in relative, "Example missing README: " + sketch)
    example_count = len(sketches)
    print(f"PASS package {version}: {len(files)} files match checkout, {example_count} documented examples, "
          f"{link_count} local links, {include_count} private includes")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package", type=Path, help="Path to the Arduino library ZIP")
    parser.add_argument("--library-root", type=Path, default=Path(__file__).resolve().parent.parent,
                        help="Source checkout to compare (default: parent of tools)")
    args = parser.parse_args()
    check(args.package, args.library_root.resolve())
