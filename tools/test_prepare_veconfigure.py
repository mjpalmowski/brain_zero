import hashlib
import json
from pathlib import Path
import struct
import tempfile
import unittest

from export_compatibility import MAGIC, Reader, SETTING_FIELDS, export_compatibility
from prepare_veconfigure import prepare_veconfigure


def profile_bytes(*, assistant_bytes=2816):
    firmware = 2623563
    version = struct.pack("<I6x", firmware)
    unit_info = bytes((47, 1, 1, 1, 0, 0, 0, 0, 0xE0, 0))
    flag_access = bytes(range(80))
    grid = bytes.fromhex("0300027f")
    rows = [[0] * 6 for _ in range(192)]
    rows[0] = [0, 0, 1, 0, 15, 0]
    rows[2] = [-100, 0, 5760, 4800, 6400, 0x81]
    rows[3] = [-100, 0, 5520, 4800, 6400, 0]
    rows[4] = [-10, 0, 500, 0, 700, 0]
    rows[12] = [-100, 0, 65520, 65436, 100, 0]
    descriptor_bytes = b"".join(struct.pack("<hhHHHB", *row) for row in rows)
    # The legacy source may have distinct unit and schema firmware words.
    layout = struct.pack("<BIIBBHI", 47, 1234567, firmware, 3, 2, len(grid), assistant_bytes)
    profile = {
        "format": 1, "model": 47, "unit_firmware": 1234567, "schema_firmware": firmware,
        "descriptor_firmware": firmware, "nominal_voltage": 48,
        "descriptor_sha256": hashlib.sha256(layout + version + unit_info + flag_access + descriptor_bytes).hexdigest(),
        "version_reply": version.hex(), "unit_info": unit_info.hex(), "schema_version": 3, "unit_version": 2,
        "grid_bytes": len(grid), "assistant_bytes": assistant_bytes, "flag_access": flag_access.hex(),
        "setting_fields": list(SETTING_FIELDS), "settings": rows,
    }
    return json.dumps({"profile": profile, "grid": grid.hex()}).encode()


class PrepareVeconfigureTest(unittest.TestCase):
    def test_preserves_descriptors_and_reports_exact_file_offsets(self):
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder)
            raw = profile_bytes()
            report = prepare_veconfigure(raw, output)
            data = (output / "synthetic.rvsc").read_bytes()
            self.assertEqual(json.loads((output / "settings-map.json").read_text(encoding="utf-8")), report)

        source = json.loads(raw)
        replay = export_compatibility(data)
        self.assertEqual(replay["grid"], source["grid"])
        self.assertEqual([list(row) for row in replay["profile"]["settings"]], source["profile"]["settings"])
        self.assertEqual(replay["profile"]["schema_version"], 4)
        self.assertEqual(replay["profile"]["unit_version"], 3)
        self.assertEqual(replay["profile"]["unit_firmware"], 2623563)
        self.assertEqual(report["source"]["descriptor_sha256"], source["profile"]["descriptor_sha256"])
        self.assertEqual(report["synthetic_file"]["descriptor_sha256"], replay["profile"]["descriptor_sha256"])

        self.assertEqual(len(report["settings"]), 192)
        for setting, row in zip(report["settings"], source["profile"]["settings"]):
            offsets = setting["file_offsets"]
            self.assertEqual(struct.unpack_from("<hhHHH", data, offsets["descriptor"]), tuple(row[:5]))
            self.assertEqual(data[offsets["access"]], row[5])
            self.assertEqual(struct.unpack_from("<H", data, offsets["value"])[0], row[2])
        absorption = report["settings"][2]
        self.assertEqual(absorption["access"], {"raw": 129, "authorization_bits": 1, "restart_bit": True})
        self.assertEqual(absorption["scaled"], {"encoding": "unsigned", "minimum": 48, "maximum": 64, "default": "288/5"})
        self.assertEqual(report["settings"][12]["scaled"]["minimum"], -1)
        self.assertEqual(report["settings"][0]["flag_mask"], 15)
        self.assertFalse(report["settings"][191]["descriptor_present"])

        reader = Reader(data)
        self.assertEqual(reader.blob(), MAGIC)
        reader.section(b"Mk2vscInfo")
        reader.section(b"BareSettingInfo")
        unit = reader.section(b"BareSettingData")
        self.assertEqual(unit.number(4), 3)
        self.assertEqual(unit.take(4), bytes(4))
        unit.take(11)
        unit.blob(6)
        self.assertEqual(unit.take(10)[8], 0xF0)
        self.assertEqual(unit.blob(), b"BZSIM000000")
        self.assertEqual(unit.take(18), bytes(18))  # Zero credentials and timestamps.
        unit.blob(384)
        self.assertEqual(unit.take(unit.remaining), struct.pack("<HBH", 0, 255, 2816))
        self.assertEqual(reader.remaining, 0)

    def test_maximum_assistant_area_uses_multiple_blank_runs(self):
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder)
            report = prepare_veconfigure(profile_bytes(assistant_bytes=65536), output)
            data = (output / "synthetic.rvsc").read_bytes()
        at = report["synthetic_file"]["file_offsets"]["assistant_runs"]
        self.assertEqual(data[at:-4], struct.pack("<HBHHBH", 0, 255, 65472, 0, 255, 64))
        self.assertEqual(export_compatibility(data)["profile"]["assistant_bytes"], 65536)

    def test_rejects_unvalidated_bundle_before_writing_files(self):
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder)
            bundle = json.loads(profile_bytes())
            bundle["profile"]["settings"][2][2] += 1
            with self.assertRaisesRegex(ValueError, "checksum"):
                prepare_veconfigure(json.dumps(bundle).encode(), output)
            self.assertEqual(list(output.iterdir()), [])


if __name__ == "__main__":
    unittest.main()
