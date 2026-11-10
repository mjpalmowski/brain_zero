"""Extract MultiPlus-II compatibility data from a local VEConfigure backup."""

import argparse
from fractions import Fraction
import hashlib
import json
from pathlib import Path
import struct


MAGIC = b"VEConfig setting section file"
MAX_BACKUP_BYTES = 128 * 1024
SETTING_COUNT = 192
SETTING_FIELDS = ("scale", "offset", "default", "minimum", "maximum", "access")
STANDALONE = bytes((1, 1, 1, 0, 0, 0, 0))
# The 230 V MultiPlus-II product prefixes recognized by brain_zero.
BATTERY_VOLTAGES = {
    12: {2604, 2605, 2630},
    24: {2611, 2615, 2696, 2697},
    48: {2601, 2602, 2619, 2621, 2623, 2625, 2626, 2627, 2628, 2629,
         2636, 2637, 2682, 2683, 2684, 2698, 2699},
}


class Reader:
    def __init__(self, data):
        self.data = data
        self.offset = 0

    @property
    def remaining(self):
        return len(self.data) - self.offset

    def take(self, length):
        if not 0 <= length <= self.remaining:
            raise ValueError("Invalid or truncated VEConfigure data")
        start = self.offset
        self.offset += length
        return self.data[start:self.offset]

    def number(self, length):
        return int.from_bytes(self.take(length), "little")

    def blob(self, expected=None):
        length = self.number(2)
        if expected is not None and length != expected:
            raise ValueError("Unexpected VEConfigure field length")
        return self.take(length)

    def section(self, expected):
        start = self.offset
        if self.blob() != expected:
            raise ValueError("Unexpected VEConfigure section layout")
        end = self.number(4)
        body = self.take(end - self.offset - 4)
        checked = self.data[start:end - 4]
        padded = checked + bytes(-len(checked) % 4)
        checksum = sum(word for word, in struct.iter_unpack("<I", padded)) & 0xFFFFFFFF
        if self.number(4) != checksum:
            raise ValueError("VEConfigure section checksum mismatch")
        return Reader(body)


def voltage_limits(setting):
    scale, offset, _, minimum, maximum, _ = setting
    if minimum > maximum:
        minimum -= 65536 if minimum >= 32768 else 0
        maximum -= 65536 if maximum >= 32768 else 0
    if scale < 0:
        return Fraction(minimum + offset, -scale), Fraction(maximum + offset, -scale)
    if scale > 0:
        return (minimum + offset) * scale, (maximum + offset) * scale
    return minimum, maximum


def export_compatibility(data):
    """Build the offline simulator fixture from schema and topology metadata."""
    if len(data) > MAX_BACKUP_BYTES:
        raise ValueError("VEConfigure backup exceeds the size limit")
    file = Reader(data)
    if file.blob(len(MAGIC)) != MAGIC:
        raise ValueError("Expected a VEConfigure settings backup (.rvsc)")
    producer = file.section(b"Mk2vscInfo")
    schema = file.section(b"BareSettingInfo")
    unit = file.section(b"BareSettingData")
    if file.remaining:
        raise ValueError("Expected a backup for one standalone inverter")
    if producer.number(4) != 1 or not producer.blob() or producer.remaining:
        raise ValueError("Unsupported VEConfigure producer layout")

    schema_version, schema_firmware, model = schema.number(4), schema.number(4), schema.number(1)
    records = schema.blob(SETTING_COUNT * 10)
    flag_access = schema.blob(80)
    access = schema.blob(SETTING_COUNT)
    grid = schema.blob()
    if schema.remaining or not grid:
        raise ValueError("Expected complete grid capability descriptors")
    settings = [(*row, permission) for row, permission in zip(struct.iter_unpack("<hhHHH", records), access)]

    unit_version = unit.number(4)
    if (schema_version, unit_version) not in ((3, 2), (4, 3)):
        raise ValueError("Unsupported VEConfigure schema/unit layout")
    unit.take(4)  # Unique unit ID stays local.
    unit_firmware = unit.number(4)
    if unit.take(1 if unit_version == 2 else 3)[0] != (1 if unit_version == 2 else 255):
        raise ValueError("Unexpected VEConfigure unit header")
    version = unit.take(4) + unit.blob(6)
    unit_info = unit.take(10)
    if (unit_info[0] != model or unit_info[1:8] != STANDALONE or unit_info[8] & 0x0F or unit_info[9]):
        raise ValueError("Expected a backup for one standalone inverter")
    firmware = int.from_bytes(version[:4], "little")
    if schema_firmware != firmware:
        raise ValueError("The settings schema and firmware reply disagree")
    prefix = firmware // 1000
    nominal_voltage = next((voltage for voltage, prefixes in BATTERY_VOLTAGES.items() if prefix in prefixes), None)
    if nominal_voltage is None:
        raise ValueError("Expected a recognized 230 V MultiPlus-II firmware")
    for setting in settings[2:4]:
        minimum, maximum = voltage_limits(setting)
        if minimum != nominal_voltage or not minimum < maximum <= nominal_voltage * Fraction(4, 3):
            raise ValueError("Charger descriptors disagree with the inverter battery voltage")

    if not 1 <= len(unit.blob()) <= 15:  # Serial number stays local.
        raise ValueError("Unexpected VEConfigure serial field length")
    credential_count = unit.number(2)
    if credential_count > 7:
        raise ValueError("Unexpected VEConfigure credential count")
    for _ in range(credential_count):
        unit.blob(8)  # Access credentials stay local.
    unit.take(16)  # Timestamps stay local.
    unit.blob(SETTING_COUNT * 2)  # Saved settings stay local.
    assistant_bytes = 0
    while unit.remaining:
        literal_length = len(unit.blob())
        marker, fill_length = unit.number(1), unit.number(2)
        if marker != 255 and fill_length:
            raise ValueError("Invalid VEConfigure Assistant fill marker")
        assistant_bytes += literal_length + fill_length
    if not 0 < assistant_bytes <= 65536 or assistant_bytes % 64:
        raise ValueError("Invalid VEConfigure Assistant area size")

    layout = struct.pack("<BIIBBHI", model, unit_firmware, schema_firmware, schema_version,
                         unit_version, len(grid), assistant_bytes)
    descriptors = b"".join(struct.pack("<hhHHHB", *row) for row in settings)
    fingerprint = hashlib.sha256(layout + version + unit_info + flag_access + descriptors).hexdigest()
    profile = {
        "format": 1,
        "model": model,
        "unit_firmware": unit_firmware,
        "schema_firmware": schema_firmware,
        "descriptor_firmware": int.from_bytes(version[:3] + version[7:8], "little"),
        "nominal_voltage": nominal_voltage,
        "descriptor_sha256": fingerprint,
        "version_reply": version.hex(),
        "unit_info": unit_info.hex(),
        "schema_version": schema_version,
        "unit_version": unit_version,
        "grid_bytes": len(grid),
        "assistant_bytes": assistant_bytes,
        "flag_access": flag_access.hex(),
        "setting_fields": list(SETTING_FIELDS),
        "settings": settings,
    }
    return {"profile": profile, "grid": grid.hex()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rvsc", type=Path, help="Local Remote VEConfigure backup")
    parser.add_argument("--output", type=Path, required=True, help="New file for the public compatibility JSON")
    args = parser.parse_args()
    try:
        with args.rvsc.open("rb") as backup:
            data = backup.read(MAX_BACKUP_BYTES + 1)
        bundle = export_compatibility(data)
        with args.output.open("x", encoding="utf-8", newline="\n") as output:
            output.write(json.dumps(bundle, indent=2) + "\n")
    except (OSError, ValueError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
