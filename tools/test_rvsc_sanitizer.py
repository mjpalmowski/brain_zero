"""Exercise the shipped browser parser against the independent Python exporter."""

import base64
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import unittest

from export_compatibility import MAGIC, MAX_BACKUP_BYTES, Reader, export_compatibility
from prepare_veconfigure import build_synthetic_rvsc
from test_intake_compatibility import ASSISTANT, CREDENTIAL, SERIAL, blob, private_backup, section
from test_prepare_veconfigure import profile_bytes
from validate_compatibility import validate_profile


HTML = Path(__file__).with_name("rvsc-sanitizer.html")


def script_text():
    html = HTML.read_text(encoding="utf-8")
    return re.search(r'<script type="module" id="sanitizer-script">([\s\S]*?)</script>', html)[1]


def sections(raw):
    reader = Reader(raw)
    assert reader.blob() == MAGIC
    return [reader.section(name).data for name in (b"Mk2vscInfo", b"BareSettingInfo", b"BareSettingData")]


def rebuild(parts):
    output = bytearray(blob(MAGIC))
    for name, payload in zip((b"Mk2vscInfo", b"BareSettingInfo", b"BareSettingData"), parts):
        section(output, name, payload)
    return bytes(output)


def legacy_private_backup():
    producer, schema, unit = sections(private_backup())
    schema = struct.pack("<I", 3) + schema[4:]
    # Unit v2 uses a one-byte header instead of the v3 three-byte header.
    unit = struct.pack("<I", 2) + unit[4:12] + b"\x01" + unit[15:]
    return rebuild((producer, schema, unit))


def voltage_backup(voltage, prefix):
    bundle = json.loads(profile_bytes())
    profile = bundle["profile"]
    firmware = prefix * 1000 + 563
    profile["schema_firmware"] = profile["unit_firmware"] = firmware
    profile["version_reply"] = struct.pack("<I6x", firmware).hex()
    profile["nominal_voltage"] = voltage
    for setting_id, default in ((2, voltage * 120), (3, voltage * 115)):
        profile["settings"][setting_id] = [-100, 0, default, voltage * 100, voltage * 400 // 3, 0]
    return build_synthetic_rvsc(bundle)[0]


class BrowserSanitizerTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if shutil.which("node") is None:
            raise RuntimeError("Node.js is required to test the browser RVSC sanitizer.")

    def convert(self, payloads):
        # Execute the exact embedded module shipped to users, with no document or
        # browser state. Node's WebCrypto provides the same SHA-256 API.
        driver = r"""
import {readFileSync} from 'node:fs';
const source = JSON.parse(readFileSync(0, 'utf8'));
const module = await import('data:text/javascript;base64,' + source.script);
const results = [];
for (const data of source.payloads) {
  try { results.push({ok: true, value: await module.sanitizeRvsc(new Uint8Array(Buffer.from(data, 'base64')))}); }
  catch (error) { results.push({ok: false, error: error.message}); }
}
process.stdout.write(JSON.stringify(results));
"""
        data = {"script": base64.b64encode(script_text().encode()).decode(),
                "payloads": [base64.b64encode(payload).decode() for payload in payloads]}
        result = subprocess.run(["node", "--input-type=module", "-e", driver],
                                input=json.dumps(data), text=True, capture_output=True, timeout=30, check=True)
        self.assertEqual(result.stderr, "")
        for secret in (SERIAL, CREDENTIAL, ASSISTANT, b"PRIVATE-TIMEDATA"):
            self.assertNotIn(secret.decode(), result.stdout)
            self.assertNotIn(secret.hex(), result.stdout)
        return json.loads(result.stdout)

    def test_private_fields_are_removed_and_export_matches_python_for_both_layouts_and_voltages(self):
        producer, schema, unit = sections(private_backup())
        distinct_unit_firmware = rebuild((producer, schema, unit[:8] + struct.pack("<I", 1234567) + unit[12:]))
        payloads = [private_backup(), legacy_private_backup(), distinct_unit_firmware,
                    voltage_backup(12, 2604), voltage_backup(24, 2611)]
        results = self.convert(payloads)
        for payload, result in zip(payloads, results):
            with self.subTest(schema=export_compatibility(payload)["profile"]["schema_version"],
                              voltage=export_compatibility(payload)["profile"]["nominal_voltage"]):
                self.assertTrue(result["ok"], result)
                # Tuple-valued Python descriptor rows normalize to JSON arrays.
                expected = json.loads(json.dumps(export_compatibility(payload)))
                self.assertEqual(result["value"], expected)
                normalized, _ = validate_profile(json.dumps(result["value"]).encode())
                self.assertEqual(normalized, expected)

    def test_invalid_checksums_truncation_section_bounds_and_private_layouts_reject(self):
        raw = private_backup()
        checksum = bytearray(raw)
        checksum[-1] ^= 1
        section_bounds = bytearray(raw)
        first_end = 2 + len(MAGIC) + 2 + len(b"Mk2vscInfo")
        struct.pack_into("<I", section_bounds, first_end, 0xffffffff)
        parts = sections(raw)
        invalid_topology = bytearray(parts[2])
        unit = Reader(parts[2])
        unit.take(19)
        unit.blob(6)
        info_at = unit.offset
        invalid_topology[info_at + 1] = 2
        invalid_credentials = bytearray(parts[2])
        unit.take(10)
        unit.blob()
        struct.pack_into("<H", invalid_credentials, unit.offset, 8)
        invalid_assistants = bytearray(parts[2])
        invalid_assistants[-3:] = b"\x00\x01\x00"  # Invalid non-255 fill marker.
        mismatched_voltage = json.loads(profile_bytes())
        mismatched_voltage["profile"]["schema_firmware"] = 2604563
        mismatched_voltage["profile"]["version_reply"] = struct.pack("<I6x", 2604563).hex()
        invalid = [b"", b"not an rvsc", raw[:-1], raw + b"extra unit", bytes(checksum),
                   bytes(section_bounds), bytes(MAX_BACKUP_BYTES + 1),
                   rebuild((parts[0], parts[1], bytes(invalid_topology))),
                   rebuild((parts[0], parts[1], bytes(invalid_credentials))),
                   rebuild((parts[0], parts[1], bytes(invalid_assistants))),
                   build_synthetic_rvsc(mismatched_voltage)[0]]
        results = self.convert(invalid)
        self.assertTrue(all(not result["ok"] for result in results), results)

    def test_csp_blocks_network_and_embedded_module_hash_matches(self):
        html = HTML.read_text(encoding="utf-8")
        script = script_text()
        digest = base64.b64encode(hashlib.sha256(script.encode()).digest()).decode()
        csp = re.search(r'http-equiv="Content-Security-Policy" content="([^"]+)"', html)[1]
        self.assertIn("script-src 'sha256-" + digest + "'", csp)
        for directive in ("default-src 'none'", "connect-src 'none'", "form-action 'none'", "base-uri 'none'", "object-src 'none'"):
            self.assertIn(directive, csp)
        self.assertEqual(len(re.findall(r"<script\b", html)), 1)
        self.assertNotRegex(html, r"(?i)<(?:script|link|img|iframe|form)\b[^>]*(?:src|href|action)=")
        self.assertNotRegex(script, r"\b(?:fetch|XMLHttpRequest|WebSocket|EventSource|sendBeacon|importScripts)\b")
        self.assertNotIn("file.name", script)
        self.assertIn('anchor.download = "compatibility.json"', script)
        self.assertIn('input.value = ""', script)
        self.assertRegex(html, r'<input id="backup"[^>]* hidden>')


if __name__ == "__main__":
    unittest.main()
