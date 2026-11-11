import hashlib
import json
from pathlib import Path
import struct
import tempfile
import unittest

import publish_firmware as release


TAG = "firmware-20261111.1"
BUILD = "github-ota-20261111.1"


def application(*, build=BUILD, private=False):
    header = bytearray(24)
    header[0], header[1], header[12], header[23] = 0xE9, 1, 9, 1
    data = bytes.fromhex("3254cdab") + b"\0" * 12 + build.encode() + b"\0cross-site\0"
    if private:
        data += b"server-intake-20261111.1-site\0"
    value = bytes(header) + struct.pack("<II", 0x3C000020, len(data)) + data
    checksum = 0xEF
    for byte in data:
        checksum ^= byte
    value += b"\0" * ((15 - len(value)) % 16) + bytes((checksum,))
    return value + hashlib.sha256(value).digest()


class PublishTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.image = self.root / release.DEFAULT_IMAGE
        self.image.parent.mkdir(parents=True)
        self.image.write_bytes(application())
        self.source = self.root / "firmware/vebus_listener/src/diagnostics.h"
        self.source.parent.mkdir(parents=True)
        self.source.write_text(f'constexpr const char *BUILD = "{BUILD}";\n'
                               'constexpr uint32_t RELEASE_VERSION = 2026111101;\n', encoding="utf-8")

    def validate(self):
        return release.validate_image(self.root, self.image, TAG)

    def test_accepts_only_valid_calendar_tags(self):
        self.assertEqual(release.tag_version(TAG), 2026111101)
        self.assertEqual(release.tag_version("firmware-20280229.99"), 2028022999)
        for tag in ("firmware-20260229.1", "firmware-20261111.0", "firmware-20261111.01",
                    "firmware-20261111.100", "firmware-20261111.1-site", "other-20261111.1"):
            with self.subTest(tag=tag), self.assertRaises(ValueError):
                release.tag_version(tag)

    def test_validates_public_build_and_rejects_private_paths_secrets_and_mismatch(self):
        firmware = self.validate()
        self.assertEqual(firmware.sha256, hashlib.sha256(application()).hexdigest())
        other = self.image.parent.parent / "listener_site/firmware.bin"
        other.parent.mkdir()
        other.write_bytes(application())
        with self.assertRaisesRegex(ValueError, "canonical"):
            release.validate_image(self.root, other, TAG)
        self.image.write_bytes(application(private=True))
        with self.assertRaisesRegex(ValueError, "private"):
            self.validate()
        self.image.write_bytes(application(build="old-build"))
        with self.assertRaisesRegex(ValueError, "identifier"):
            self.validate()
        self.image.write_bytes(application())
        include = self.root / "firmware/vebus_listener/include"
        include.mkdir()
        (include / "wifi_secrets.h").write_text("private", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "secret"):
            self.validate()

    def test_rejects_bootloader_wrong_chip_truncation_corruption_and_merged_flash(self):
        valid = application()
        wrong_chip = bytearray(valid)
        wrong_chip[12] = 0
        wrong_magic = bytearray(valid)
        wrong_magic[32] = 0
        corrupt_checksum = bytearray(valid)
        corrupt_checksum[-33] ^= 1
        corrupt_hash = bytearray(valid)
        corrupt_hash[-1] ^= 1
        for raw in (b"\xff" * 4096, bytes(wrong_chip), bytes(wrong_magic), valid[:36],
                    bytes(corrupt_checksum), bytes(corrupt_hash), valid + b"\xff" * 32):
            with self.subTest(size=len(raw)), self.assertRaises(ValueError):
                release.validate_application(raw)

    def payload(self, firmware, *, draft=True, extra=False, bad_hash=False):
        download_tag = "untagged-676167a2250f4448398d" if draft else TAG
        assets = [{"name": "firmware.bin", "size": firmware.size, "state": "uploaded",
                   "digest": "sha256:" + ("0" * 64 if bad_hash else firmware.sha256),
                   "browser_download_url": f"https://github.com/{release.REPOSITORY}/releases/download/{download_tag}/firmware.bin"}]
        if extra:
            assets.append({"name": "SHA256SUMS.txt"})
        return {"tag_name": TAG, "draft": draft, "assets": assets}

    def runner(self, firmware, *, extra=False, bad_hash=False, dirty=False):
        calls = []

        def run(args, *, cwd):
            calls.append(args)
            if args[:4] == ["git", "remote", "get-url", "origin"]:
                return f"https://github.com/{release.REPOSITORY}.git"
            if args[:2] == ["git", "status"]:
                return " M source.cpp" if dirty else ""
            if args[:2] == ["git", "ls-remote"]:
                return ""
            if args == ["git", "rev-parse", "HEAD"]:
                return "a" * 40
            if args[:3] == ["gh", "release", "create"]:
                body = Path(args[args.index("--notes-file") + 1]).read_text(encoding="utf-8")
                self.assertIn(firmware.sha256, body)
                self.assertIn("brain_zero Rev C", body)
                self.assertEqual(args[4], str(self.image))
                self.assertNotIn("*", " ".join(args))
                return ""
            if args[:3] == ["gh", "release", "view"]:
                return json.dumps({"apiUrl": f"https://api.github.com/repos/{release.REPOSITORY}/releases/441463957"})
            if args[:2] == ["gh", "api"]:
                self.assertEqual(args[2], f"repos/{release.REPOSITORY}/releases/441463957")
                published = any(call[:3] == ["gh", "release", "edit"] for call in calls)
                return json.dumps(self.payload(firmware, draft=not published, extra=extra, bad_hash=bad_hash))
            if args[:3] == ["gh", "release", "edit"]:
                return ""
            raise AssertionError(args)

        return calls, run

    def test_creates_draft_with_one_bin_and_publishes_after_hash_verification(self):
        firmware = self.validate()
        calls, run = self.runner(firmware)
        checked = []
        url = release.publish(self.root, firmware, "Changes", run=run, server_check=checked.append)
        self.assertEqual(checked, [firmware])
        self.assertTrue(url.endswith(TAG))
        create = next(call for call in calls if call[:3] == ["gh", "release", "create"])
        self.assertIn("--draft", create)
        self.assertEqual(len([arg for arg in create if arg.endswith(".bin")]), 1)
        self.assertEqual(sum(call[:2] == ["gh", "api"] for call in calls), 2)
        self.assertTrue(any(call[:3] == ["gh", "release", "edit"] for call in calls))

    def test_draft_download_url_is_temporary_and_published_url_must_use_tag(self):
        firmware = self.validate()
        value = self.payload(firmware)
        release.verify_release(value, firmware, draft=True)
        value["draft"] = False
        with self.assertRaisesRegex(ValueError, "verification"):
            release.verify_release(value, firmware, draft=False)
        value = self.payload(firmware)
        value["assets"][0]["browser_download_url"] = value["assets"][0]["browser_download_url"].replace(
            release.REPOSITORY, "someone/other-repo")
        with self.assertRaisesRegex(ValueError, "verification"):
            release.verify_release(value, firmware, draft=True)
        release.verify_release(self.payload(firmware, draft=False), firmware, draft=False)

    def test_draft_api_route_requires_official_host_repo_and_numeric_id(self):
        self.assertEqual(release.release_route({"apiUrl":
            f"https://api.github.com/repos/{release.REPOSITORY}/releases/441463957"}),
            f"repos/{release.REPOSITORY}/releases/441463957")
        for url in (f"https://api.github.com/repos/{release.REPOSITORY}/releases/tags/{TAG}",
                    "https://api.github.com/repos/someone/other-repo/releases/441463957",
                    f"https://unrelated.example/repos/{release.REPOSITORY}/releases/441463957"):
            with self.subTest(url=url), self.assertRaisesRegex(ValueError, "numeric API"):
                release.release_route({"apiUrl": url})

    def test_leaves_draft_unpublished_on_extra_attachment_or_hash_mismatch(self):
        firmware = self.validate()
        for options in ({"extra": True}, {"bad_hash": True}):
            calls, run = self.runner(firmware, **options)
            with self.subTest(options=options), self.assertRaisesRegex(ValueError, "verification"):
                release.publish(self.root, firmware, "Changes", run=run, server_check=lambda _: None)
            self.assertFalse(any(call[:3] == ["gh", "release", "edit"] for call in calls))

    def test_source_and_server_guards_run_before_draft_creation(self):
        firmware = self.validate()
        calls, run = self.runner(firmware, dirty=True)
        with self.assertRaisesRegex(ValueError, "Commit"):
            release.publish(self.root, firmware, "Changes", run=run, server_check=lambda _: None)
        self.assertFalse(any(call[0] == "gh" for call in calls))
        calls, run = self.runner(firmware)
        def unavailable(_):
            raise ValueError("Server mismatch")
        with self.assertRaisesRegex(ValueError, "Server mismatch"):
            release.publish(self.root, firmware, "Changes", run=run, server_check=unavailable)
        self.assertFalse(any(call[0] == "gh" for call in calls))


if __name__ == "__main__":
    unittest.main()
