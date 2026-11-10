"""Validate an untrusted compatibility profile for offline simulator review."""

from fractions import Fraction
import hashlib
import json
import re
import struct

from export_compatibility import BATTERY_VOLTAGES, SETTING_COUNT, SETTING_FIELDS, STANDALONE, voltage_limits


# A VEConfigure grid descriptor can be up to 65,535 bytes before hex encoding.
MAX_PROFILE_BYTES = 256 * 1024
PROFILE_FIELDS = {
    "format", "model", "unit_firmware", "schema_firmware", "descriptor_firmware",
    "nominal_voltage", "descriptor_sha256", "version_reply", "unit_info",
    "schema_version", "unit_version", "grid_bytes", "assistant_bytes",
    "flag_access", "setting_fields", "settings",
}
DESCRIPTOR = struct.Struct("<hhHHHB")
DESCRIPTOR_BOUNDS = (
    (-32768, 32767), (-32768, 32767), (0, 65535), (0, 65535), (0, 65535), (0, 255),
)
HEX = re.compile(r"[0-9a-fA-F]+")


def _unique_keys(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("The compatibility JSON contains duplicate keys.")
        result[key] = value
    return result


def _parse_integer(text):
    if len(text.lstrip("-")) > 10:
        raise ValueError("The compatibility JSON contains an oversized integer.")
    return int(text)


def _reject_noninteger(_text):
    raise ValueError("The compatibility JSON requires whole numbers for numeric fields.")


def _integer(profile, key, lower, upper):
    value = profile[key]
    if type(value) is not int or not lower <= value <= upper:
        raise ValueError(f"The {key} field is outside its allowed integer range.")
    return value


def _hex(value, size, field):
    if not isinstance(value, str) or len(value) != size * 2 or not HEX.fullmatch(value):
        raise ValueError(f"The {field} field has invalid hex data or length.")
    return bytes.fromhex(value)


def _encode_descriptors(rows):
    if not isinstance(rows, list) or len(rows) != SETTING_COUNT:
        raise ValueError("The compatibility profile must contain 192 setting descriptors.")
    encoded = bytearray()
    for row in rows:
        if (not isinstance(row, list) or len(row) != len(DESCRIPTOR_BOUNDS) or
                any(type(value) is not int or not lower <= value <= upper
                    for value, (lower, upper) in zip(row, DESCRIPTOR_BOUNDS))):
            raise ValueError("A setting descriptor does not match the six-field wire format.")
        encoded.extend(DESCRIPTOR.pack(*row))
    return encoded


def _number(value):
    return int(value) if value.denominator == 1 else float(value)


def validate_profile(raw: bytes) -> tuple[dict, dict]:
    """Return a normalized simulator bundle and its review metadata."""
    if len(raw) > MAX_PROFILE_BYTES:
        raise ValueError("The compatibility JSON exceeds the 256 KiB size limit.")
    try:
        bundle = json.loads(raw.decode("utf-8-sig"), object_pairs_hook=_unique_keys,
                            parse_int=_parse_integer, parse_float=_reject_noninteger,
                            parse_constant=_reject_noninteger)
    except (UnicodeDecodeError, json.JSONDecodeError, RecursionError):
        raise ValueError("The attachment is not valid UTF-8 compatibility JSON.") from None
    if not isinstance(bundle, dict) or set(bundle) != {"profile", "grid"}:
        raise ValueError("The attachment must contain only the profile and grid fields.")
    profile = bundle["profile"]
    if not isinstance(profile, dict) or set(profile) != PROFILE_FIELDS:
        raise ValueError("The compatibility profile has missing or unexpected fields.")
    _integer(profile, "format", 1, 1)
    if profile["setting_fields"] != list(SETTING_FIELDS):
        raise ValueError("The compatibility profile uses an unsupported descriptor format.")

    schema_version = _integer(profile, "schema_version", 3, 4)
    unit_version = _integer(profile, "unit_version", 2, 3)
    if (schema_version, unit_version) not in ((3, 2), (4, 3)):
        raise ValueError("The VEConfigure schema and unit versions are incompatible.")
    model = _integer(profile, "model", 0, 255)
    unit_firmware = _integer(profile, "unit_firmware", 0, 0xFFFFFFFF)
    firmware = _integer(profile, "schema_firmware", 0, 0xFFFFFFFF)
    descriptor_firmware = _integer(profile, "descriptor_firmware", 0, 0xFFFFFFFF)
    grid_bytes = _integer(profile, "grid_bytes", 1, 65535)
    assistant_bytes = _integer(profile, "assistant_bytes", 64, 65536)
    if assistant_bytes % 64:
        raise ValueError("The VEConfigure Assistant area must contain whole records.")

    version_reply = _hex(profile["version_reply"], 10, "version_reply")
    unit_info = _hex(profile["unit_info"], 10, "unit_info")
    flags = _hex(profile["flag_access"], 80, "flag_access")
    grid = _hex(bundle["grid"], grid_bytes, "grid")
    declared_fingerprint = _hex(profile["descriptor_sha256"], 32, "descriptor_sha256")
    if unit_info[0] != model or unit_info[1:8] != STANDALONE or unit_info[8] & 0x0F or unit_info[9]:
        raise ValueError("The profile must describe a single standalone VE.Bus inverter.")
    if (firmware != int.from_bytes(version_reply[:4], "little") or
            descriptor_firmware != int.from_bytes(version_reply[:3] + version_reply[7:8], "little")):
        raise ValueError("The profile firmware metadata disagrees with the version reply.")
    nominal_voltage = _integer(profile, "nominal_voltage", 12, 48)
    if firmware // 1000 not in BATTERY_VOLTAGES.get(nominal_voltage, set()):
        raise ValueError("The firmware and battery voltage do not match a recognized 230 V MultiPlus-II.")

    rows = profile["settings"]
    descriptors = _encode_descriptors(rows)
    limits = {}
    for setting_id, name in ((2, "absorption_v"), (3, "float_v"), (4, "charge_current_a")):
        lower, upper = voltage_limits(rows[setting_id])
        if setting_id in (2, 3) and not (
                lower == nominal_voltage and lower < upper <= nominal_voltage * Fraction(4, 3)):
            raise ValueError("The charger voltage limits disagree with the nominal battery voltage.")
        if setting_id == 4 and not 0 <= lower <= upper:
            raise ValueError("The charge current limits are invalid.")
        limits[name] = {"minimum": _number(lower), "maximum": _number(upper)}

    # The established profile fingerprint covers grid length, not its contents.
    layout = struct.pack("<BIIBBHI", model, unit_firmware, firmware, schema_version,
                         unit_version, grid_bytes, assistant_bytes)
    fingerprint = hashlib.sha256(layout + version_reply + unit_info + flags + descriptors).digest()
    if declared_fingerprint != fingerprint:
        raise ValueError("The descriptor checksum does not match the profile data.")
    normalized = {"profile": {**profile, "version_reply": version_reply.hex(),
                              "unit_info": unit_info.hex(), "flag_access": flags.hex(),
                              "descriptor_sha256": fingerprint.hex()}, "grid": grid.hex()}
    report = {
        "firmware": firmware,
        "unit_firmware": unit_firmware,
        "descriptor_firmware": descriptor_firmware,
        "model": model,
        "nominal_voltage": nominal_voltage,
        "descriptor_sha256": fingerprint.hex(),
        "grid_sha256": hashlib.sha256(grid).hexdigest(),
        "grid_bytes": grid_bytes,
        "assistant_bytes": assistant_bytes,
        "settings_limits": limits,
    }
    return normalized, report
