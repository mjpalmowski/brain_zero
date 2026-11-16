#!/usr/bin/env python3
"""Bounded read-only JK BLE qualification probe. No Home Assistant actions.

Requires the Pi's existing dbus-python and PyGObject/BlueZ, not pip packages.
Command/identity offsets and the 300-byte checksum match the installation's
syssi/esphome-jk-bms revision 6b5a25aa43fb044c2a9592a5e87f06f8edc42554.
Only zero-value read opcodes 0x97 and 0x96 can be sent. Status read requires an
exact operator-supplied model/hardware/firmware match. Never changes settings or MOSFETs.
Only validated type02 telemetry can be written to a private 0600 /tmp capture;
identity/settings records are never saved or printed.
Never print raw notifications, device addresses, serials, or passcodes.
Advertisement names are limited to a whitelisted model or a family hint.
"""
import argparse
import json
import os
import re
import signal
import sys
import time
from pathlib import Path

import dbus
from dbus.mainloop.glib import DBusGMainLoop
from gi.repository import GLib

BLUEZ = "org.bluez"
DEVICE = "org.bluez.Device1"
ADAPTER = "org.bluez.Adapter1"
SERVICE = "org.bluez.GattService1"
CHARACTERISTIC = "org.bluez.GattCharacteristic1"
PROPERTIES = "org.freedesktop.DBus.Properties"
FFE0 = "0000ffe0-0000-1000-8000-00805f9b34fb"
FFE1 = "0000ffe1-0000-1000-8000-00805f9b34fb"
HEADER = bytes.fromhex("55aaeb90")
# Exact tag-1.5.0 write_register(0x97, 0, 0), CRC=sum(first19)&255.
READ_DEVICE_INFO = bytes.fromhex("aa5590eb97000000000000000000000000000011")
READ_CELL_INFO = bytes.fromhex("aa5590eb96000000000000000000000000000010")
assert len(READ_DEVICE_INFO) == 20
assert READ_DEVICE_INFO[4] == 0x97 and not any(READ_DEVICE_INFO[5:19])
assert READ_DEVICE_INFO[-1] == sum(READ_DEVICE_INFO[:-1]) & 255
assert len(READ_CELL_INFO) == 20 and READ_CELL_INFO[4] == 0x96
assert not any(READ_CELL_INFO[5:19])
assert READ_CELL_INFO[-1] == sum(READ_CELL_INFO[:-1]) & 255


class ProbeStop(Exception):
    pass


def clean_field(raw, kind):
    value = raw.split(b"\x00", 1)[0].decode("ascii", "ignore").strip()
    if kind == "model":
        return value if re.fullmatch(r"JK[-_A-Za-z0-9. /]{1,13}", value) else None
    return value if re.fullmatch(r"[0-9][A-Za-z0-9._-]{0,7}", value) else None


def telemetry_candidates(raw):
    """Evidence only: do not claim universal automatic format detection."""
    candidates = []
    for fmt, maximum, cell_offset, scalar_offset in (
            ("JK02_24S", 24, 0, 0), ("JK02_32S", 32, 16, 32)):
        mask = int.from_bytes(raw[54 + cell_offset:58 + cell_offset], "little")
        cells = [int.from_bytes(raw[6 + 2*i:8 + 2*i], "little") / 1000.0
                 for i in range(maximum)]
        active = [i for i in range(maximum) if mask & (1 << i)]
        volts = int.from_bytes(raw[118 + scalar_offset:122 + scalar_offset], "little") / 1000.0
        soc = raw[141 + scalar_offset]
        if (not active or mask >> maximum or soc > 100 or not 5 < volts < 170):
            continue
        if not all(0.5 <= cells[i] <= 5.0 for i in active):
            continue
        if any(cells[i] > 0.1 for i in range(maximum) if i not in active):
            continue
        if abs(sum(cells[i] for i in active) - volts) > 1.0:
            continue
        candidates.append({"format": fmt, "enabled_cells": len(active),
                           "pack_voltage_v": round(volts, 3), "soc_percent": soc})
    return candidates


def advertised_name_hint(name):
    # Many advertisements contain an address/serial as their name. Never echo it.
    if re.fullmatch(r"JK-(?:BD|B|PB)[0-9A-Z]{3,11}", name):
        return name
    if re.match(r"^JK", name, re.IGNORECASE):
        return "JK (suffix redacted)"
    if re.match(r"^(PACE|PC[-_ ]?)", name, re.IGNORECASE):
        return "PACE/PC (suffix redacted)"
    return "other (name redacted)" if name else "unnamed"


def safe_bluez_reason(message):
    # Fixed BlueZ error slugs only; exception text can contain an address.
    allowed = (
        "br-connection-profile-unavailable", "br-connection-page-timeout",
        "br-connection-create-socket", "br-connection-key-missing",
        "br-connection-canceled", "le-connection-abort-by-local",
        "le-connection-abort-by-remote", "le-connection-complete-timeout",
        "le-connection-canceled", "le-connection-establishment-failed",
        "le-connection-create-socket", "le-connection-no-response",
        "Operation already in progress", "Software caused connection abort",
        "Connection timed out", "Connection refused", "Not connected",
    )
    return next((reason for reason in allowed if reason in message), "other_redacted")


def run():
    parser = argparse.ArgumentParser(description="Read-only JK BLE telemetry probe")
    parser.add_argument("--expected-address", required=True, help="Known approved JK BLE address; never output")
    parser.add_argument("--model", required=True, help="Required exact model before status reads")
    parser.add_argument("--hardware", required=True, help="Required exact hardware before status reads")
    parser.add_argument("--firmware", required=True, help="Required exact firmware before status reads")
    parser.add_argument("--telemetry-output", required=True, help="New private /tmp/brain_zero_jk_*.bin capture")
    parser.add_argument("--frames", type=int, default=3, choices=range(1,7))
    parser.add_argument("--check", action="store_true", help="Validate arguments only; no radio or file writes")
    args = parser.parse_args()
    expected = args.expected_address.upper() if args.expected_address else None
    if expected and not re.fullmatch(r"(?:[0-9A-F]{2}:){5}[0-9A-F]{2}", expected):
        raise SystemExit("Invalid expected-address syntax")
    capture_path = Path(args.telemetry_output)
    if (not capture_path.is_absolute() or capture_path.resolve().parent != Path("/tmp").resolve()
            or not re.fullmatch(r"brain_zero_jk_[A-Za-z0-9_-]+[.]bin", capture_path.name)):
        raise SystemExit("Telemetry output must be a new /tmp/brain_zero_jk_*.bin file")
    if not re.fullmatch(r"JK[-_A-Za-z0-9. /]{1,13}", args.model):
        raise SystemExit("Invalid required model")
    if not all(re.fullmatch(r"[0-9][A-Za-z0-9._-]{0,7}", x) for x in (args.hardware,args.firmware)):
        raise SystemExit("Invalid required hardware or firmware")
    if not 6 <= int(re.match(r"[0-9]+", args.hardware).group()) < 11:
        raise SystemExit("This probe qualifies JK02_24S hardware 6 through 10 only")
    expected_identity = {"model": args.model, "hardware": args.hardware, "firmware": args.firmware}
    if args.check:
        return {"status": "preflight_ok", "identity_required": expected_identity,
                "telemetry_only_capture": True, "output_already_exists": capture_path.exists()}
    capture_fd = None
    status_requested = False
    captured_frames = 0
    started = time.monotonic()
    deadline = started + 38.0  # reserve up to7seconds for cleanup; hard cap45.
    report = {"probe": "jk_telemetry_read_only", "status": "starting", "stage": "starting",
              "read_commands": ["0x97 identity", "0x96 cells/status after exact identity match"],
              "scan_seconds": 12, "candidate_count": 0,
              "checksum_valid_records": 0, "checksum_failures": 0,
              "record_types": {}, "identity": None,
              "telemetry_format_candidates": [], "telemetry": [], "captured_frames": 0,
              "cleanup": {"scan_stopped": True, "notifications_stopped": True,
                          "disconnected": True}}
    bus = None
    adapter = None
    target_path = None
    notify_path = None
    own_scan = False
    own_connection = False
    connect_attempted = False
    own_notify = False
    signal_match = None
    assembly = bytearray()
    context = GLib.MainContext.default()

    def hard_stop(signum, frame):
        # No exception messages/tracebacks can expose BlueZ addresses or payloads.
        report["status"] = "hard_timeout"
        report["cleanup"]["hard_timeout"] = True
        print(json.dumps(report, sort_keys=True), flush=True)
        os._exit(124)

    def stop_early(signum, frame):
        raise ProbeStop("interrupted")

    signal.signal(signal.SIGALRM, hard_stop)
    signal.signal(signal.SIGTERM, stop_early)
    signal.signal(signal.SIGINT, stop_early)
    signal.alarm(45)

    def pump(seconds, predicate=None):
        until = min(deadline, time.monotonic() + seconds)
        while time.monotonic() < until:
            if predicate and predicate():
                return True
            while context.pending():
                context.iteration(False)
            time.sleep(0.02)
        while context.pending():
            context.iteration(False)
        return bool(predicate and predicate())

    def call_timeout(cap):
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise ProbeStop("work_deadline")
        return min(cap, remaining)

    def objects():
        return dbus.Interface(bus.get_object(BLUEZ, "/"),
                              "org.freedesktop.DBus.ObjectManager").GetManagedObjects(timeout=call_timeout(2))

    def props(path, interface):
        return dbus.Interface(bus.get_object(BLUEZ, path), PROPERTIES).GetAll(interface, timeout=call_timeout(1.5))

    def erase(data):
        for i in range(len(data)):
            data[i] = 0
        data.clear()

    def on_value(interface, changed, invalidated, path=None):
        nonlocal captured_frames
        if interface != CHARACTERISTIC or str(path) != notify_path or "Value" not in changed:
            return
        chunk = bytearray(changed["Value"])
        raw = bytearray()
        try:
            if chunk.startswith(HEADER):
                erase(assembly)
            if not assembly and not chunk.startswith(HEADER):
                return
            if len(assembly) + len(chunk) > 400:
                erase(assembly)
                report["oversize_dropped"] = report.get("oversize_dropped", 0) + 1
                return
            assembly.extend(chunk)
            if len(assembly) < 300:
                return
            raw.extend(assembly[:300])
            erase(assembly)
            if raw[:4] != HEADER or (sum(raw[:299]) & 255) != raw[299]:
                report["checksum_failures"] += 1
                return
            report["checksum_valid_records"] += 1
            kind = "0x%02x" % raw[4]
            report["record_types"][kind] = report["record_types"].get(kind, 0) + 1
            if raw[4] == 3:
                model = clean_field(raw[6:22], "model")
                if model:
                    report["identity"] = {"model": model,
                                          "hardware": clean_field(raw[22:30], "version"),
                                          "firmware": clean_field(raw[30:38], "version")}
            elif (raw[4] == 2 and status_requested and report["identity"] == expected_identity
                  and captured_frames < args.frames and capture_fd is not None):
                candidates = telemetry_candidates(raw)
                report["telemetry_format_candidates"] = candidates
                if not any(c["format"] == "JK02_24S" for c in candidates):
                    report["layout_rejected"] = True
                    return
                # This is the ONLY write of received packet bytes. Its enclosing
                # guard permits checksum-valid status type02, never identity03.
                if os.write(capture_fd, raw) != len(raw):
                    report["capture_error"] = "short_write"
                    return
                captured_frames += 1
                report["captured_frames"] = captured_frames
                cells = [int.from_bytes(raw[6+2*i:8+2*i], "little") / 1000 for i in range(24)]
                mask = int.from_bytes(raw[54:58], "little")
                enabled = [v for i,v in enumerate(cells) if mask & (1<<i)]
                report["telemetry"].append({
                    "counter": raw[5], "soc": raw[141],
                    "voltage_v": int.from_bytes(raw[118:122], "little") / 1000,
                    "current_a": int.from_bytes(raw[126:130], "little", signed=True) / 1000,
                    "min_cell_v": min(enabled), "max_cell_v": max(enabled),
                    "delta_cell_v": round(max(enabled)-min(enabled), 6),
                    "cells": len(enabled),
                    "capacity_ah": int.from_bytes(raw[146:150], "little") / 1000,
                })
        finally:
            erase(raw)
            erase(chunk)

    try:
        DBusGMainLoop(set_as_default=True)
        bus = dbus.SystemBus()
        all_objects = objects()
        adapters = [str(p) for p, interfaces in all_objects.items()
                    if ADAPTER in interfaces and bool(interfaces[ADAPTER].get("Powered", False))]
        if len(adapters) != 1:
            report["status"] = "need_one_powered_adapter"
            return report
        adapter_path = adapters[0]
        adapter = dbus.Interface(bus.get_object(BLUEZ, adapter_path), ADAPTER)
        report["stage"] = "setting_LE_discovery_filter"
        # Per-DBus-client discovery filter; automatically released on client exit.
        # The old C8 JK module can otherwise be observed through the BR/EDR bearer.
        adapter.SetDiscoveryFilter(dbus.Dictionary({"Transport": dbus.String("le")}, signature="sv"),
                                   timeout=call_timeout(2))
        report["scan_transport"] = "le"
        # Discovery sessions belong to this DBus client; StopDiscovery ends only ours.
        report["stage"] = "scanning"
        adapter.StartDiscovery(timeout=call_timeout(2))
        own_scan = True
        report["cleanup"]["scan_stopped"] = False
        pump(report["scan_seconds"])
        # Snapshot while our discovery lease is active: transient Device1 objects
        # can disappear after StopDiscovery/client release.
        all_objects = objects()
        adapter.StopDiscovery(timeout=call_timeout(2))
        own_scan = False
        report["cleanup"]["scan_stopped"] = True
        candidates = []
        advertisements = []
        for path, interfaces in all_objects.items():
            info = interfaces.get(DEVICE)
            if not info or str(info.get("Adapter", "")) != adapter_path:
                continue
            name = str(info.get("Name", ""))
            address = str(info.get("Address", "")).upper()
            uuids = {str(u).lower() for u in info.get("UUIDs", [])}
            if "RSSI" in info:
                advertisements.append({"name_hint": advertised_name_hint(name),
                                       "service_uuids": sorted(uuids)})
            jk_name = bool(re.match(r"^JK(?:[-_ ]|BMS|$)", name, re.IGNORECASE))
            if expected:
                # An address read from the user's working bridge configuration
                # identifies the approved target even if its advertisement lacks
                # a name or UUID. FFE0/FFE1 are still required after discovery,
                # before the one read-only command can be sent.
                qualified = address == expected
            else:
                # FFE0 alone also belongs to non-JK devices. Never connect on it alone.
                qualified = jk_name and (not uuids or FFE0 in uuids)
            if qualified:
                candidates.append((str(path), info))
        report["advertisements"] = advertisements[:30]
        report["candidate_count"] = len(candidates)
        if len(candidates) != 1:
            report["status"] = "need_one_unambiguous_JK_candidate"
            return report
        target_path, info = candidates[0]
        report["stage"] = "candidate_found"
        if bool(info.get("Connected", False)):
            report["status"] = "already_connected_refused"
            return report
        device = dbus.Interface(bus.get_object(BLUEZ, target_path), DEVICE)
        connect_attempted = True
        report["cleanup"]["disconnected"] = False
        report["stage"] = "connecting"
        device.Connect(timeout=call_timeout(6))
        own_connection = True
        report["stage"] = "resolving_GATT"
        if not pump(8, lambda: bool(props(target_path, DEVICE).get("ServicesResolved", False))):
            report["status"] = "GATT_resolution_timeout"
            return report
        all_objects = objects()
        report["stage"] = "checking_characteristics"
        services = {str(p) for p, i in all_objects.items()
                    if SERVICE in i and str(i[SERVICE].get("Device", "")) == target_path
                    and str(i[SERVICE].get("UUID", "")).lower() == FFE0}
        candidates = []
        for path, interfaces in all_objects.items():
            char = interfaces.get(CHARACTERISTIC)
            if char and str(char.get("Service", "")) in services and str(char.get("UUID", "")).lower() == FFE1:
                flags = {str(flag) for flag in char.get("Flags", [])}
                candidates.append((str(path), flags, bool(char.get("Notifying", False))))
        report["service_uuid"] = FFE0 if services else None
        report["characteristics"] = [{"uuid": FFE1, "properties": sorted(flags)}
                                     for path, flags, notifying in candidates]
        writers = [(p, f, n) for p, f, n in candidates if "write-without-response" in f]
        notifiers = [(p, f, n) for p, f, n in candidates if "notify" in f]
        if len(writers) != 1 or len(notifiers) != 1 or notifiers[0][2]:
            report["status"] = "ambiguous_or_in_use_GATT_refused"
            return report
        write_path = writers[0][0]
        notify_path = notifiers[0][0]
        signal_match = bus.add_signal_receiver(on_value, signal_name="PropertiesChanged",
                                               dbus_interface=PROPERTIES, path_keyword="path")
        notifier = dbus.Interface(bus.get_object(BLUEZ, notify_path), CHARACTERISTIC)
        report["stage"] = "subscribing"
        notifier.StartNotify(timeout=call_timeout(2))
        own_notify = True
        report["cleanup"]["notifications_stopped"] = False
        writer = dbus.Interface(bus.get_object(BLUEZ, write_path), CHARACTERISTIC)
        report["stage"] = "requesting_identity"
        writer.WriteValue(dbus.Array(READ_DEVICE_INFO, signature="y"),
                          dbus.Dictionary({"type": dbus.String("command")}, signature="sv"), timeout=call_timeout(2))
        report["command_sent"] = True
        report["stage"] = "awaiting_identity"
        pump(5, lambda: report["identity"] is not None)
        if report["identity"] != expected_identity:
            report["status"] = "identity_mismatch_or_missing"
            return report
        capture_fd = os.open(capture_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
        report["stage"] = "requesting_telemetry"
        status_requested = True
        writer.WriteValue(dbus.Array(READ_CELL_INFO, signature="y"),
                          dbus.Dictionary({"type": dbus.String("command")}, signature="sv"), timeout=call_timeout(2))
        report["telemetry_request_sent"] = True
        report["stage"] = "awaiting_telemetry"
        pump(12, lambda: captured_frames >= args.frames or "capture_error" in report)
        report["captured_bytes"] = captured_frames * 300
        report["status"] = "telemetry_received" if captured_frames else "no_valid_telemetry"
        report["stage"] = "completed"
        return report
    except ProbeStop as exc:
        report["status"] = "work_deadline" if exc.args == ("work_deadline",) else "interrupted"
        return report
    except dbus.DBusException as exc:
        # DBus exception text may include addresses; emit only its standardized name.
        report["status"] = "dbus_error"
        report["error_name"] = exc.get_dbus_name()
        report["error_reason"] = safe_bluez_reason(exc.get_dbus_message())
        return report
    except Exception:
        report["status"] = "probe_error"
        return report
    finally:
        def cleanup_call(action, key):
            try:
                if time.monotonic() - started < 43.5:
                    action()
                    report["cleanup"][key] = True
            except Exception:
                report["cleanup"][key] = False
        if own_notify and bus and notify_path:
            cleanup_call(lambda: dbus.Interface(bus.get_object(BLUEZ, notify_path), CHARACTERISTIC)
                         .StopNotify(timeout=1), "notifications_stopped")
        if signal_match:
            try:
                signal_match.remove()
            except Exception:
                pass
        if (own_connection or connect_attempted) and bus and target_path:
            cleanup_call(lambda: dbus.Interface(bus.get_object(BLUEZ, target_path), DEVICE)
                         .Disconnect(timeout=1), "disconnected")
            # Old JK modules may finish disconnection before BlueZ's method
            # reply, yielding a timeout despite Connected already being false.
            for _ in range(12):
                if time.monotonic() - started >= 43.5:
                    break
                try:
                    connected = dbus.Interface(bus.get_object(BLUEZ, target_path), PROPERTIES).Get(
                        DEVICE, "Connected", timeout=0.5)
                    if not connected:
                        report["cleanup"]["disconnected"] = True
                        break
                except dbus.DBusException as exc:
                    if exc.get_dbus_name() in ("org.freedesktop.DBus.Error.UnknownObject",
                                               "org.bluez.Error.DoesNotExist"):
                        report["cleanup"]["disconnected"] = True
                        break
                time.sleep(0.05)
        if own_scan and adapter:
            cleanup_call(lambda: adapter.StopDiscovery(timeout=1), "scan_stopped")
        erase(assembly)
        if capture_fd is not None:
            os.fsync(capture_fd)
            os.close(capture_fd)
        report["elapsed_seconds"] = round(time.monotonic() - started, 2)
        signal.alarm(0)


if __name__ == "__main__":
    result = run()
    print(json.dumps(result, sort_keys=True), flush=True)
    sys.exit(0 if result["status"] in ("telemetry_received", "preflight_ok") else 2)
