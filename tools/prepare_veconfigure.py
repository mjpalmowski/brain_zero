"""Build an offline VEConfigure fixture and setting map from a public profile."""

import argparse
from fractions import Fraction
import hashlib
import json
from pathlib import Path
import struct

from export_compatibility import MAGIC, SETTING_FIELDS, export_compatibility
from validate_compatibility import MAX_PROFILE_BYTES, validate_profile


SIMULATED_SERIAL = b"BZSIM000000"
FLAG_WORDS = (0, 1, 60, 61, 82)
FIELD_NAMES = {2: "Absorption voltage", 3: "Float voltage", 4: "Charge current"}
FIELD_UNITS = {2: "V", 3: "V", 4: "A"}
DESCRIPTOR = struct.Struct("<hhHHH")
WORD = struct.Struct("<H")


def _blob(data):
    return WORD.pack(len(data)) + data


def _section(file, name, payload):
    start = len(file)
    file.extend(_blob(name))
    payload_start = len(file) + 4
    file.extend(struct.pack("<I", payload_start + len(payload) + 4))
    file.extend(payload)
    checked = bytes(file[start:])
    padded = checked + bytes(-len(checked) % 4)
    checksum = sum(word for word, in struct.iter_unpack("<I", padded)) & 0xFFFFFFFF
    file.extend(struct.pack("<I", checksum))
    return payload_start


def _fixture_values(settings, grid):
    values = [row[2] for row in settings]
    code = values[81]
    if len(grid) < 5 or grid[2] != 3 or grid[4] != 8 or not 0 <= code < grid[3] <= 64:
        return values, False
    table_end = 5 + 2 * grid[3]
    if table_end > len(grid):
        return values, False
    start, = WORD.unpack_from(grid, 5 + 2 * code)
    if not table_end <= start <= len(grid) - 19:
        return values, False
    support = int.from_bytes(grid[start + 11:start + 19], "little")
    present = [128 + bit for bit in range(1, 63) if support & (1 << bit)]
    defaults_at = start + 19
    if defaults_at + 2 * len(present) >= len(grid):
        return values, False

    # VEConfigure treats the grid region as a selected profile and a paired
    # validity word, not as 64 independent descriptor defaults. Use only the
    # profile's public defaults in this synthetic, non-deployable fixture.
    values[129:191] = [65535] * 62
    for index, setting_id in enumerate(present):
        values[setting_id], = WORD.unpack_from(grid, defaults_at + 2 * index)
    generation = (values[128] + 256) & 0xFF00
    if generation == values[191] & 0xFF00:
        generation = (values[191] + 256) & 0xFF00
    values[128] = generation | code
    values[191] = generation | ((code + grid[start]) & 255)
    return values, True


def build_synthetic_rvsc(bundle):
    """Encode a validated bundle with defaults and an artificial standalone unit."""
    profile = bundle["profile"]
    settings = profile["settings"]
    grid = bytes.fromhex(bundle["grid"])
    flags = bytes.fromhex(profile["flag_access"])
    version = bytes.fromhex(profile["version_reply"])
    unit_info = bytearray.fromhex(profile["unit_info"])
    unit_info[8] = 0xF0  # No Assistants are installed in this synthetic unit.
    firmware = profile["schema_firmware"]

    schema_head = struct.pack("<IIB", 4, firmware, profile["model"])
    descriptors = _blob(b"".join(DESCRIPTOR.pack(*row[:5]) for row in settings))
    flag_access = _blob(flags)
    access = _blob(bytes(row[5] for row in settings))
    schema = schema_head + descriptors + flag_access + access + _blob(grid)

    unit_head = struct.pack("<I4xI", 3, firmware) + b"\xff\x01\x01" + version[:4] + _blob(version[4:])
    unit_head += unit_info + _blob(SIMULATED_SERIAL) + bytes(18)  # Zero credentials and timestamps.
    words, _ = _fixture_values(settings, grid)
    values = _blob(struct.pack("<192H", *words))
    assistants = b"".join(struct.pack("<HBH", 0, 255, min(profile["assistant_bytes"] - at, 65472))
                          for at in range(0, profile["assistant_bytes"], 65472))

    file = bytearray(_blob(MAGIC))
    _section(file, b"Mk2vscInfo", struct.pack("<I", 1) + _blob(b"1.32"))
    schema_offset = _section(file, b"BareSettingInfo", schema)
    unit_offset = _section(file, b"BareSettingData", unit_head + values + assistants)
    layout = {
        "descriptors": schema_offset + len(schema_head) + 2,
        "flag_access": schema_offset + len(schema_head) + len(descriptors) + 2,
        "setting_access": schema_offset + len(schema_head) + len(descriptors) + len(flag_access) + 2,
        "grid": schema_offset + len(schema_head) + len(descriptors) + len(flag_access) + len(access) + 2,
        "values": unit_offset + len(unit_head) + 2,
        "assistant_runs": unit_offset + len(unit_head) + len(values),
    }
    return bytes(file), layout


def _access(byte):
    return {"raw": byte, "authorization_bits": byte & 0x7F, "restart_bit": bool(byte & 0x80)}


def _decoded_range(row):
    scale, offset, default, minimum, maximum, _ = row
    if not scale or minimum == maximum == 0xFFFF:
        return None
    wrapped = minimum > maximum
    if wrapped and not minimum >= 0x8000 > maximum:
        return None

    def decode(word):
        raw = word - 65536 if wrapped and word >= 0x8000 else word
        value = (raw + offset) * scale if scale > 0 else Fraction(raw + offset, -scale)
        return int(value) if value.denominator == 1 else str(value)

    return {"encoding": "signed-wrap inference" if wrapped else "unsigned",
            "minimum": decode(minimum), "maximum": decode(maximum), "default": decode(default)}


def _settings_map(bundle, details, image, layout):
    source = bundle["profile"]
    replay = export_compatibility(image)
    fixture = replay["profile"]
    if (replay["grid"] != bundle["grid"] or
            [list(row) for row in fixture["settings"]] != source["settings"] or
            any(fixture[key] != source[key] for key in
                ("flag_access", "version_reply", "model", "assistant_bytes", "nominal_voltage"))):
        raise ValueError("The synthetic VEConfigure fixture did not preserve the source descriptors.")

    flags = bytes.fromhex(source["flag_access"])
    _, grid_defaults = _fixture_values(source["settings"], bytes.fromhex(bundle["grid"]))
    settings = []
    for setting_id, row in enumerate(source["settings"]):
        present = any(row[:5])
        setting = {
            "id": setting_id,
            "descriptor_present": present,
            "synthetic_word": WORD.unpack_from(image, layout["values"] + 2 * setting_id)[0],
            "raw": dict(zip(SETTING_FIELDS, row)),
            "access": _access(row[5]),
            "file_offsets": {
                "descriptor": layout["descriptors"] + setting_id * DESCRIPTOR.size,
                "access": layout["setting_access"] + setting_id,
                "value": layout["values"] + setting_id * WORD.size,
            },
        }
        if setting_id in FLAG_WORDS:
            block = FLAG_WORDS.index(setting_id) * 16
            setting["flag_mask"] = row[4]
            setting["flag_access"] = [_access(byte) for byte in flags[block:block + 16]]
            setting["file_offsets"]["flag_access"] = layout["flag_access"] + block
        elif present:
            setting["scaled"] = _decoded_range(row)
        if setting_id in FIELD_NAMES:
            setting.update(name=FIELD_NAMES[setting_id], unit=FIELD_UNITS[setting_id])
        settings.append(setting)

    return {
        "format": 1,
        "scope": "Descriptor map for offline compatibility review.",
        "source": details,
        "synthetic_file": {
            "name": "synthetic.rvsc",
            "sha256": hashlib.sha256(image).hexdigest(),
            "descriptor_sha256": fixture["descriptor_sha256"],
            "schema_version": fixture["schema_version"],
            "unit_version": fixture["unit_version"],
            "settings_source": "descriptor defaults with synthetic grid initialization" if grid_defaults else "descriptor defaults",
            "grid_profile_defaults_applied": grid_defaults,
            "assistant_source": "empty synthetic memory",
            "file_offsets": layout,
        },
        "notes": [
            "Offsets are zero-based byte positions in the synthetic file.",
            "Synthetic grid profile values and validity words can differ from the descriptor defaults.",
            "Scaled values are descriptor units, and fractional values are exact rational strings.",
            "Native VEConfigure controls can apply additional conversions and dependencies.",
            "Descriptors and access bits alone do not establish runtime visibility or write permission.",
        ],
        "settings": settings,
    }


def prepare_veconfigure(raw, output_dir):
    """Revalidate a profile and write the fixture and map to an existing directory."""
    bundle, details = validate_profile(raw)
    image, layout = build_synthetic_rvsc(bundle)
    report = _settings_map(bundle, details, image, layout)
    (output_dir / "synthetic.rvsc").write_bytes(image)
    (output_dir / "settings-map.json").write_text(json.dumps(report, indent=2, allow_nan=False) + "\n",
                                                  encoding="utf-8", newline="\n")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", type=Path, required=True, help="Validated public compatibility bundle")
    parser.add_argument("--output-dir", type=Path, required=True, help="Existing directory for the fixture and map")
    args = parser.parse_args()
    try:
        with args.profile.open("rb") as profile:
            prepare_veconfigure(profile.read(MAX_PROFILE_BYTES + 1), args.output_dir)
    except (OSError, ValueError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
