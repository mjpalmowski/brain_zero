"""Publish one public brain_zero Rev C OTA application as firmware.bin.

Run with --dry-run to validate locally without contacting GitHub or the server.
The release stays a draft unless its sole uploaded asset matches the local image.
"""

import argparse
from dataclasses import dataclass
from datetime import datetime
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import urllib.request


ROOT = Path(__file__).resolve().parents[1]
REPOSITORY = "mjpalmowski/brain_zero"
MAX_OTA_BYTES = 3342336
DEFAULT_IMAGE = Path("firmware/vebus_listener/.pio/build/listener/firmware.bin")


@dataclass(frozen=True)
class Firmware:
    tag: str
    image: Path
    size: int
    sha256: str
    build: str


def tag_version(tag):
    match = re.fullmatch(r"firmware-(20[2-9][0-9][0-1][0-9][0-3][0-9])\.([1-9][0-9]?)", tag)
    if not match:
        raise ValueError("Use a firmware-YYYYMMDD.N tag, with sequence 1 through 99.")
    try:
        datetime.strptime(match[1], "%Y%m%d")
    except ValueError:
        raise ValueError("The firmware tag contains an invalid date.") from None
    return int(match[1]) * 100 + int(match[2])


def validate_application(raw):
    """Accept exactly one normal, unsigned ESP32-S3 application image."""
    if (not 36 <= len(raw) <= MAX_OTA_BYTES or raw[0] != 0xE9 or
            not 1 <= raw[1] <= 16 or struct.unpack_from("<H", raw, 12)[0] != 9 or
            raw[32:36] != bytes.fromhex("3254cdab") or raw[23] not in (0, 1)):
        raise ValueError("Choose the public ESP32-S3 OTA application, not a bootloader or merged image.")
    cursor, checksum = 24, 0xEF
    for _ in range(raw[1]):
        if cursor + 8 > len(raw):
            raise ValueError("The OTA application is truncated.")
        _, length = struct.unpack_from("<II", raw, cursor)
        cursor += 8
        if cursor + length > len(raw):
            raise ValueError("The OTA application is truncated.")
        for value in raw[cursor:cursor + length]:
            checksum ^= value
        cursor += length
    checksum_at = cursor + ((15 - cursor) % 16)
    image_end = checksum_at + 1
    expected_end = image_end + (32 if raw[23] else 0)
    if expected_end != len(raw) or any(raw[cursor:checksum_at]):
        raise ValueError("The file must contain exactly one OTA application with no merged flash data.")
    if raw[checksum_at] != checksum:
        raise ValueError("The OTA application checksum is invalid.")
    if raw[23] and hashlib.sha256(raw[:image_end]).digest() != raw[image_end:]:
        raise ValueError("The OTA application's embedded SHA-256 is invalid.")


def validate_image(root, image, tag):
    version = tag_version(tag)
    root = Path(root).resolve()
    image = Path(image).resolve()
    if image != (root / DEFAULT_IMAGE).resolve():
        raise ValueError("Only the canonical public listener/firmware.bin may be released; custom and site builds stay private.")
    include = root / "firmware/vebus_listener/include"
    if any((include / name).exists() for name in ("wifi_secrets.h", "free_power_secrets.h")):
        raise ValueError("Remove private secret headers from the public release checkout and rebuild first.")
    source = (root / "firmware/vebus_listener/src/diagnostics.h").read_text(encoding="utf-8")
    builds = re.findall(r'constexpr\s+const\s+char\s*\*\s*BUILD\s*=\s*"([^"]+)"', source)
    versions = re.findall(r"constexpr\s+uint32_t\s+RELEASE_VERSION\s*=\s*(\d+)\s*;", source)
    if len(builds) != 1 or len(versions) != 1 or "-site" in builds[0]:
        raise ValueError("Use the public firmware source checkout, without private site build variants.")
    if int(versions[0]) != version or not builds[0].endswith(tag.removeprefix("firmware-")):
        raise ValueError("Update the public BUILD and RELEASE_VERSION to match the release tag, then rebuild.")
    raw = image.read_bytes()
    validate_application(raw)
    if (re.search(rb"[A-Za-z0-9_.-]+-\d{8}\.\d{1,2}-site\x00", raw) or
            (builds[0] + "\0").encode("ascii") not in raw):
        raise ValueError("The image is private or does not contain the expected public build identifier.")
    return Firmware(tag, image, len(raw), hashlib.sha256(raw).hexdigest(), builds[0])


def command(args, *, cwd=None):
    return subprocess.run(args, cwd=cwd, text=True, capture_output=True, check=True).stdout.strip()


def verify_server(firmware):
    url = f"https://brain-zero.com/firmware/{firmware.tag}/firmware.bin"
    request = urllib.request.Request(url, headers={"User-Agent": "brain_zero firmware release publisher"})
    with urllib.request.urlopen(request, timeout=20) as response:
        if response.status != 200 or response.geturl() != url:
            raise ValueError("Publish the same OTA image at its final brain-zero.com URL before releasing.")
        digest, count = hashlib.sha256(), 0
        while chunk := response.read(min(65536, firmware.size - count + 1)):
            count += len(chunk)
            if count > firmware.size:
                raise ValueError("The server's OTA image is larger than the validated local image.")
            digest.update(chunk)
        if count != firmware.size or digest.hexdigest() != firmware.sha256:
            raise ValueError("The brain-zero.com image does not match the validated local image.")


def verify_release(value, firmware, *, draft):
    expected_url = f"https://github.com/{REPOSITORY}/releases/download/{firmware.tag}/firmware.bin"
    assets = value.get("assets", [])
    if (value.get("tag_name") != firmware.tag or value.get("draft") is not draft or
            len(assets) != 1 or assets[0].get("name") != "firmware.bin" or
            assets[0].get("size") != firmware.size or assets[0].get("state") != "uploaded" or
            assets[0].get("digest") != "sha256:" + firmware.sha256 or
            assets[0].get("browser_download_url") != expected_url):
        raise ValueError("Release verification failed: exactly one matching firmware.bin asset is required.")


def publish(root, firmware, notes, *, preview=False, run=command, server_check=verify_server):
    root = Path(root)
    remote = run(["git", "remote", "get-url", "origin"], cwd=root).removesuffix(".git")
    if remote not in (f"https://github.com/{REPOSITORY}", f"git@github.com:{REPOSITORY}"):
        raise ValueError("Publish from the official brain_zero checkout.")
    if run(["git", "status", "--porcelain", "--untracked-files=no"], cwd=root):
        raise ValueError("Commit the release source and documentation before publishing.")
    if run(["git", "ls-remote", "--tags", "origin", "refs/tags/" + firmware.tag], cwd=root):
        raise ValueError("The release tag already exists. Choose a fresh version rather than replacing a release.")
    revision = run(["git", "rev-parse", "HEAD"], cwd=root)
    if not re.fullmatch(r"[0-9a-f]{40,64}", revision):
        raise ValueError("Could not identify the release source commit.")
    server_check(firmware)
    body = notes.strip() + ("\n\nOTA application for brain_zero Rev C devices only. "
                            "Other hardware should compile from the repository.\n\n"
                            f"`firmware.bin` SHA-256: `{firmware.sha256}`\n")
    with tempfile.TemporaryDirectory(prefix="brain-zero-release-") as temporary:
        notes_path = Path(temporary) / "release-notes.md"
        notes_path.write_text(body, encoding="utf-8")
        create = ["gh", "release", "create", firmware.tag, str(firmware.image), "--repo", REPOSITORY,
                  "--draft", "--target", revision, "--title", f"brain_zero OTA · {firmware.tag}",
                  "--notes-file", str(notes_path)]
        if preview:
            create.append("--prerelease")
        run(create, cwd=root)
    route = f"repos/{REPOSITORY}/releases/tags/{firmware.tag}"
    verify_release(json.loads(run(["gh", "api", route], cwd=root)), firmware, draft=True)
    # No wildcard upload or extra attachments: publication happens only after
    # GitHub reports the exact local image as the sole asset, including its digest.
    run(["gh", "release", "edit", firmware.tag, "--repo", REPOSITORY, "--draft=false"], cwd=root)
    verify_release(json.loads(run(["gh", "api", route], cwd=root)), firmware, draft=False)
    return f"https://github.com/{REPOSITORY}/releases/tag/{firmware.tag}"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tag", help="firmware-YYYYMMDD.N")
    parser.add_argument("--firmware", type=Path, default=ROOT / DEFAULT_IMAGE)
    parser.add_argument("--notes", type=Path, help="UTF-8 release notes; required for publishing")
    parser.add_argument("--preview", action="store_true", help="Publish as a prerelease")
    parser.add_argument("--dry-run", action="store_true", help="Validate locally without network or release mutations")
    args = parser.parse_args()
    try:
        firmware = validate_image(ROOT, args.firmware, args.tag)
        result = {"tag": firmware.tag, "build": firmware.build, "asset": "firmware.bin",
                  "bytes": firmware.size, "sha256": firmware.sha256}
        if not args.dry_run:
            if args.notes is None:
                raise ValueError("Supply --notes with a UTF-8 release description.")
            result["release"] = publish(ROOT, firmware, args.notes.read_text(encoding="utf-8"), preview=args.preview)
        print(json.dumps(result, indent=2))
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        if isinstance(error, subprocess.CalledProcessError):
            parser.exit(1, "Release command failed. Check GitHub access; any created draft is left unpublished.\n")
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
