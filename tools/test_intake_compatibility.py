from contextlib import redirect_stderr
from io import BytesIO, StringIO
import json
import os
from pathlib import Path
import stat
import struct
import tempfile
import unittest
from unittest.mock import patch
from urllib.error import URLError
from urllib.request import Request
from zipfile import ZIP_BZIP2, ZIP_DEFLATED, ZIP_STORED, ZipFile, ZipInfo

from export_compatibility import MAGIC, MAX_BACKUP_BYTES, Reader
import intake_compatibility as intake
from prepare_veconfigure import build_synthetic_rvsc, prepare_veconfigure
from test_prepare_veconfigure import profile_bytes
from validate_compatibility import MAX_PROFILE_BYTES, validate_profile


BASE = "https://github.com/user-attachments/files/1234/"
SERIAL = b"PRIVATE-SERIAL1"
CREDENTIAL = b"SECRETS!"
ASSISTANT = b"PRIVATE-ASSISTANT-CONTENTS"


def body(filename="compatibility.json", *, field=intake.PROFILE_FIELD, firmware="2623563"):
    return ("### Inverter model\n\nMultiPlus-II 48/5000/70-50\n\n"
            f"### Firmware shown by Victron\n\n{firmware}\n\n"
            f"### {field}\n\n[{filename}]({BASE}{filename})\n")


def blob(data):
    return struct.pack("<H", len(data)) + data


def section(file, name, payload):
    start = len(file)
    file.extend(blob(name))
    file.extend(struct.pack("<I", len(file) + 4 + len(payload) + 4))
    file.extend(payload)
    checked = bytes(file[start:])
    checked += bytes(-len(checked) % 4)
    checksum = sum(word for word, in struct.iter_unpack("<I", checked)) & 0xffffffff
    file.extend(struct.pack("<I", checksum))


def private_backup():
    """A valid synthetic backup with private source fields deliberately filled."""
    bundle = json.loads(profile_bytes())
    image, _ = build_synthetic_rvsc(bundle)
    reader = Reader(image)
    assert reader.blob() == MAGIC
    producer = reader.section(b"Mk2vscInfo").data
    schema = reader.section(b"BareSettingInfo").data
    unit = reader.section(b"BareSettingData")
    unit.take(19)
    unit.blob(6)
    unit.take(10)
    head = bytearray(unit.data[:unit.offset])
    head[4:8] = b"\xde\xad\xbe\xef"
    assistants = ASSISTANT.ljust(bundle["profile"]["assistant_bytes"], b"!")
    payload = bytes(head) + blob(SERIAL) + struct.pack("<H", 1) + blob(CREDENTIAL)
    payload += b"PRIVATE-TIMEDATA" + blob(struct.pack("<192H", *([0x4321] * 192)))
    payload += blob(assistants) + b"\xff\x00\x00"
    file = bytearray(blob(MAGIC))
    section(file, b"Mk2vscInfo", producer)
    section(file, b"BareSettingInfo", schema)
    section(file, b"BareSettingData", payload)
    return bytes(file)


def zipped(data, name="Any inverter settings.RVSC", *, compression=ZIP_DEFLATED, extra=None, mode=None):
    output = BytesIO()
    with ZipFile(output, "w", compression=compression) as archive:
        member = ZipInfo(name)
        member.compress_type = compression
        if mode is not None:
            member.create_system = 3
            member.external_attr = mode << 16
        archive.writestr(member, data)
        if extra is not None:
            archive.writestr("extra.txt", extra)
    return output.getvalue()


class SubmissionTest(unittest.TestCase):
    def test_new_and_legacy_fields_accept_only_fixed_name_sanitized_profile(self):
        for field in intake.PROFILE_FIELDS:
            with self.subTest(field=field):
                self.assertEqual(intake.submission(body(field=field)), (BASE + "compatibility.json", 2623563))
        self.assertEqual(intake.submission(body(firmware="_No response_"))[1], None)

    def test_original_backups_and_identifying_filenames_are_rejected(self):
        for name in ("Backup.RVSC", "backup.zip", "PRIVATE-SERIAL.json", "compatibility(1).json"):
            with self.subTest(name=name), self.assertRaises(ValueError) as result:
                intake.submission(body(name))
            self.assertNotIn("PRIVATE-SERIAL", str(result.exception))

    def test_rejects_ambiguous_fields_links_and_bad_firmware(self):
        invalid = [
            body() + "\n### Downloaded compatibility profile\n\n[other](" + BASE + "other.json)\n",
            body() + "\n### " + intake.PROFILE_FIELD + "\n\n[other](" + BASE + "other.json)\n",
            body() + "\n[other](" + BASE + "other.json)\n",
            body("settings.txt"), body(firmware="563"),
            body().replace("MultiPlus-II 48/5000/70-50", "_No response_"),
        ]
        for value in invalid:
            with self.subTest(body=value), self.assertRaises(ValueError):
                intake.submission(value)

    def test_url_and_decoded_filename_cannot_escape_github_attachments(self):
        invalid = [
            "https://example.com/compatibility.json",
            BASE.replace("github.com", "github.com.example") + "compatibility.json",
            BASE.replace("https://", "http://") + "compatibility.json",
            BASE + "compatibility.json?x=1", BASE + "compatibility.json#fragment",
            BASE + "%2e%2e%2fsettings.rvsc", BASE + "%5csettings.zip",
            BASE + "c%3asettings.zip", BASE + "settings%00.zip",
        ]
        for url in invalid:
            with self.subTest(url=url), self.assertRaises(ValueError):
                intake.attachment_kind(url)

    def test_issue_trigger_recognizes_both_field_headings_without_compatibility_title(self):
        with tempfile.TemporaryDirectory() as folder:
            event = Path(folder) / "event.json"
            for field in intake.PROFILE_FIELDS:
                event.write_text(json.dumps({"action": "opened", "issue": {"number": 12,
                    "title": "Inverter support", "body": body(field=field)}}), encoding="utf-8")
                environment = {"GITHUB_REPOSITORY": "example/brain_zero", "GITHUB_EVENT_NAME": "issues",
                               "GITHUB_EVENT_PATH": str(event)}
                with patch.dict(os.environ, environment, clear=True), patch.object(intake, "process", return_value=0) as process:
                    with patch("sys.argv", ["intake", "--output-dir", str(Path(folder) / "output")]):
                        self.assertEqual(intake.main(), 0)
                    process.assert_called_once()


class DownloadTest(unittest.TestCase):
    def test_download_is_anonymous_and_reads_only_the_type_specific_bound(self):
        for extension, limit in (("json", MAX_PROFILE_BYTES), ("rvsc", MAX_BACKUP_BYTES), ("zip", intake.MAX_ZIP_BYTES)):
            response = BytesIO(b"profile")
            reads = []
            original_read = response.read
            response.read = lambda amount: (reads.append(amount), original_read(amount))[1]
            with patch.dict(os.environ, {"GITHUB_TOKEN": "never-forward-this"}), patch.object(intake, "build_opener") as opener:
                opener.return_value.open.return_value = response
                self.assertEqual(intake.download_profile(BASE + "file." + extension), b"profile")
                request = opener.return_value.open.call_args.args[0]
                self.assertNotIn("Authorization", dict(request.header_items()))
                self.assertEqual(opener.return_value.open.call_args.kwargs["timeout"], 15)
                self.assertEqual(reads, [limit + 1])

    def test_download_rejects_oversize_network_failure_and_untrusted_url(self):
        with patch.object(intake, "build_opener") as opener:
            opener.return_value.open.return_value = BytesIO(b"x" * (MAX_BACKUP_BYTES + 1))
            with self.assertRaisesRegex(ValueError, "size limit"):
                intake.download_profile(BASE + "file.rvsc")
            opener.return_value.open.side_effect = URLError("private URL is not reported")
            with self.assertRaisesRegex(ValueError, "Could not download"):
                intake.download_profile(BASE + "file.zip")
            opener.reset_mock()
            with self.assertRaises(ValueError):
                intake.download_profile("https://example.com/file.json")
            opener.assert_not_called()

    def test_redirect_only_allows_https_github_object_storage(self):
        handler = intake.AttachmentRedirect()
        request = Request(BASE + "file.zip")
        target = "https://objects.githubusercontent.com/github-production-repository-file/test?sig=abc"
        redirected = handler.redirect_request(request, None, 302, "Found", {}, target)
        self.assertEqual(redirected.full_url, target)
        self.assertNotIn("Authorization", dict(redirected.header_items()))
        for url in ("https://example.com/file", "http://objects.githubusercontent.com/file",
                    "https://objects.githubusercontent.com.example/file", "https://user@objects.githubusercontent.com/file"):
            with self.subTest(url=url), self.assertRaisesRegex(ValueError, "outside GitHub"):
                handler.redirect_request(request, None, 302, "Found", {}, url)


class SanitizeTest(unittest.TestCase):
    def test_json_stays_supported_and_raw_zip_backups_produce_the_same_profile(self):
        json_profile = profile_bytes()
        self.assertEqual(intake.sanitize_attachment(json_profile, "json"), validate_profile(json_profile))
        raw = private_backup()
        expected = intake.sanitize_attachment(raw, "rvsc")
        for compression in (ZIP_STORED, ZIP_DEFLATED):
            self.assertEqual(intake.sanitize_attachment(zipped(raw, compression=compression), "zip"), expected)
        fixture, details = expected
        encoded = json.dumps(fixture).encode()
        for secret in (SERIAL, CREDENTIAL, ASSISTANT, b"PRIVATE-TIMEDATA"):
            self.assertNotIn(secret, encoded)
            self.assertNotIn(secret.hex().encode(), encoded)
        self.assertEqual(details["firmware"], 2623563)
        self.assertEqual(fixture["profile"]["settings"][2][2], 5760)

    def test_rejects_paths_nonregular_entries_and_extra_files(self):
        for name in ("../settings.rvsc", "folder/settings.rvsc", "/settings.rvsc", "folder\\settings.rvsc",
                     "C:settings.rvsc", "settings.rvsc/", "not-a-backup.txt", "settings\x00.rvsc"):
            with self.subTest(name=name), self.assertRaises(ValueError):
                intake.sanitize_attachment(zipped(b"backup", name), "zip")
        for mode in (stat.S_IFLNK | 0o777, stat.S_IFDIR | 0o755, stat.S_IFIFO | 0o600):
            with self.subTest(mode=mode), self.assertRaisesRegex(ValueError, "regular"):
                intake.sanitize_attachment(zipped(b"backup", mode=mode), "zip")
        with self.assertRaisesRegex(ValueError, "exactly one"):
            intake.sanitize_attachment(zipped(b"backup", extra=b"private extra data"), "zip")
        output = BytesIO()
        with ZipFile(output, "w"):
            pass
        with self.assertRaisesRegex(ValueError, "exactly one"):
            intake.sanitize_attachment(output.getvalue(), "zip")

    def test_rejects_oversize_archive_and_compressed_bomb_before_reading_member(self):
        for kind, limit in (("json", MAX_PROFILE_BYTES), ("rvsc", MAX_BACKUP_BYTES), ("zip", intake.MAX_ZIP_BYTES)):
            with self.subTest(kind=kind), self.assertRaisesRegex(ValueError, "size limit"):
                intake.sanitize_attachment(b"x" * (limit + 1), kind)
        archive = zipped(b"0" * (MAX_BACKUP_BYTES + 1))
        self.assertLess(len(archive), 1024)
        with patch.object(ZipFile, "open", side_effect=AssertionError("oversized member must not be opened")):
            with self.assertRaisesRegex(ValueError, "128 KiB"):
                intake.sanitize_attachment(archive, "zip")
        with self.assertRaisesRegex(ValueError, "empty"):
            intake.sanitize_attachment(zipped(b""), "zip")

    def test_rejects_encryption_unsupported_compression_and_corruption_without_filename_leak(self):
        encrypted = bytearray(zipped(b"backup"))
        for signature, offset in ((b"PK\x03\x04", 6), (b"PK\x01\x02", 8)):
            at = encrypted.index(signature) + offset
            struct.pack_into("<H", encrypted, at, struct.unpack_from("<H", encrypted, at)[0] | 1)
        with self.assertRaisesRegex(ValueError, "Encrypted"):
            intake.sanitize_attachment(bytes(encrypted), "zip")
        with self.assertRaisesRegex(ValueError, "standard"):
            intake.sanitize_attachment(zipped(b"backup", compression=ZIP_BZIP2), "zip")
        damaged = bytearray(zipped(b"backup", name="PRIVATE-SERIAL.rvsc", compression=ZIP_STORED))
        damaged[damaged.index(b"backup")] ^= 1
        deflate = bytearray(zipped(b"backup", name="PRIVATE-SERIAL.rvsc"))
        name_bytes, extra_bytes = struct.unpack_from("<HH", deflate, 26)
        deflate[30 + name_bytes + extra_bytes] = 7  # Reserved DEFLATE block type.
        for raw in (b"not a ZIP", bytes(damaged), bytes(deflate)):
            with self.assertRaisesRegex(ValueError, "invalid, damaged or unsupported") as result:
                intake.sanitize_attachment(raw, "zip")
            self.assertNotIn("PRIVATE-SERIAL", str(result.exception))

    def test_rejects_truncated_or_invalid_backup_instead_of_treating_it_as_json(self):
        for raw in (b"{}", private_backup()[:-1]):
            for kind in ("rvsc", "zip"):
                with self.subTest(kind=kind), self.assertRaises(ValueError):
                    intake.sanitize_attachment(zipped(raw) if kind == "zip" else raw, kind)


class ProcessTest(unittest.TestCase):
    def test_only_sanitized_files_reach_artifacts_and_synthetic_processing(self):
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder) / "intake"
            profile, _ = intake.sanitize_attachment(private_backup(), "rvsc")
            payload = json.dumps(profile).encode()
            issue = {"number": 12, "body": body()}
            with patch.object(intake, "download_profile", return_value=payload):
                self.assertEqual(intake.process(issue, "example/brain_zero", output), 0)
            self.assertEqual({p.name for p in output.iterdir()}, {"profile.json", "simulator-profile.json", "report.json"})
            report = json.loads((output / "report.json").read_text())
            self.assertNotIn("attachment_url", report)
            self.assertEqual(report["attachment_type"], "json")
            self.assertEqual(report["status"], "validated")
            prepare_veconfigure((output / "profile.json").read_bytes(), output)
            for path in output.iterdir():
                data = path.read_bytes()
                for secret in (SERIAL, CREDENTIAL, ASSISTANT, b"PRIVATE-SERIAL", b"PRIVATE-TIMEDATA"):
                    self.assertNotIn(secret, data, path.name)
                    self.assertNotIn(secret.hex().encode(), data, path.name)
            self.assertIn(b"BZSIM000000", (output / "synthetic.rvsc").read_bytes())

    def test_originals_are_rejected_before_download_without_logging_name(self):
        for name in ("PRIVATE-SERIAL.rvsc", "PRIVATE-SERIAL.zip", "PRIVATE-SERIAL.json"):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as folder:
                output = Path(folder) / "intake"
                summary = Path(folder) / "summary.txt"
                stderr = StringIO()
                issue = {"number": 12, "body": body(name)}
                with patch.object(intake, "download_profile") as download, redirect_stderr(stderr):
                    with patch.dict(os.environ, {"GITHUB_STEP_SUMMARY": str(summary)}):
                        self.assertEqual(intake.process(issue, "example/brain_zero", output), 1)
                    download.assert_not_called()
                self.assertEqual({p.name for p in output.iterdir()}, {"report.json"})
                for data in ((output / "report.json").read_text(), summary.read_text(), stderr.getvalue()):
                    self.assertNotIn("PRIVATE-SERIAL", data)
                    self.assertNotIn(BASE, data)

    def test_unexpected_private_json_fields_are_rejected_and_not_republished(self):
        for field in ("serial", "credentials", "saved_settings"):
            with self.subTest(field=field), tempfile.TemporaryDirectory() as folder:
                output = Path(folder) / "intake"
                payload = json.loads(profile_bytes())
                payload['profile'][field] = "PRIVATE-SERIAL"
                with patch.object(intake, "download_profile", return_value=json.dumps(payload).encode()), redirect_stderr(StringIO()):
                    self.assertEqual(intake.process({"number": 12, "body": body()}, "example/brain_zero", output), 1)
                self.assertEqual({p.name for p in output.iterdir()}, {"report.json"})
                self.assertNotIn("PRIVATE-SERIAL", (output / "report.json").read_text())

    def test_bad_firmware_or_backup_writes_report_only(self):
        for firmware, payload in (("2623556", profile_bytes()), ("2623563", b"not a profile")):
            with self.subTest(firmware=firmware), tempfile.TemporaryDirectory() as folder:
                output = Path(folder) / "intake"
                issue = {"number": 12, "body": body(firmware=firmware)}
                with patch.object(intake, "download_profile", return_value=payload), redirect_stderr(StringIO()):
                    self.assertEqual(intake.process(issue, "example/brain_zero", output), 1)
                self.assertEqual({p.name for p in output.iterdir()}, {"report.json"})
                self.assertEqual(json.loads((output / "report.json").read_text())["status"], "rejected")


if __name__ == "__main__":
    unittest.main()
