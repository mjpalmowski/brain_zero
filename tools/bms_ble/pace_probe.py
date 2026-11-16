#!/usr/bin/env python3
"""Bounded, read-only BlueZ probe for the brain_zero PACE CAN-to-BLE adapter.

Uses the Venus Pi's existing dbus-python and GLib packages. No pairing, CAN
transmit, configuration writes, raw frame logging or device identifiers in JSON.
The only BLE writes are normal notification subscription/unsubscription.
"""
import argparse
import json
import math
import signal
import struct
import sys
import time

SERVICE_UUID = "e6c00001-b5a3-4f4f-9b8d-73d619b32720"
METADATA_UUID = "e6c00002-b5a3-4f4f-9b8d-73d619b32720"
FRAMES_UUID = "e6c00003-b5a3-4f4f-9b8d-73d619b32720"
IDS = {0x351, 0x355, 0x356, 0x359, 0x35C, 0x35E, 0x373}
MINIMUM_LENGTH = {0x351: 6, 0x355: 4, 0x356: 6, 0x359: 5,
                  0x35C: 1, 0x35E: 8, 0x373: 8}


def unsigned_delta(new, old):
    return (new - old) & 0xFFFFFFFF


def signed_delta(new, old):
    d = unsigned_delta(new, old)
    return d if d < 0x80000000 else d - 0x100000000


class Receiver:
    """Independent source-age/sequence validation before reporting CAN values."""
    def __init__(self):
        self.boot = None
        self.anchor = None
        self.last_sequence = None
        self.accepted = 0
        self.rejected = 0
        self.ids = {}
        self.latest = {}
        self.last_error = None

    def invalidate(self):
        self.anchor = None
        self.latest.clear()
        self.last_sequence = None

    def metadata(self, data, begin_ms, end_ms):
        if (len(data) != 20 or data[:4] != b"BZCB" or data[4:6] != b"\x01\x01"
                or not 0 <= end_ms - begin_ms <= 250):
            self.invalidate()
            return False
        boot, ticks, rx, dropped = struct.unpack_from("<IIIH", data, 6)
        if not boot:
            self.invalidate()
            return False
        if self.anchor and self.boot == boot:
            previous_ticks, previous_begin = self.anchor
            dt = signed_delta(ticks, previous_ticks)
            upper = end_ms - previous_begin
            lower = begin_ms - self.anchor_end
            skew = abs(upper) / 2000 + 2
            if begin_ms < self.anchor_end or dt < 0 or not lower - skew <= dt <= upper + skew:
                self.invalidate()
                return False
        if self.boot != boot:
            self.ids.clear()
            self.latest.clear()
            self.last_sequence = None
        self.boot = boot
        self.anchor = (ticks, begin_ms)
        self.anchor_end = end_ms
        self.rx = rx
        self.dropped = dropped
        return True

    def receive(self, data, now_ms):
        def refuse(why):
            self.rejected += 1
            self.last_error = why
            return False
        if not self.anchor:
            return refuse("clock_not_verified")
        if len(data) != 20 or data[0] != 1 or data[1] > 8:
            return refuse("record_shape")
        dlc, can_id = data[1], struct.unpack_from("<H", data, 2)[0]
        if can_id not in IDS:
            return refuse("id_or_dlc")
        if any(data[12 + dlc:]):
            return refuse("nonzero_padding")
        sequence, source_ms = struct.unpack_from("<II", data, 4)
        if self.last_sequence is not None and not 0 < unsigned_delta(sequence, self.last_sequence) < 0x80000000:
            return refuse("duplicate_or_old_sequence")
        ticks, local_begin = self.anchor
        if not 0 <= math.ceil(now_ms * 1000) - math.floor(self.anchor_end * 1000) < 60000000:
            return refuse("clock_anchor_expired")
        # Anchor to the start, not midpoint/end, of the read to conservatively
        # include its RTT. Add the adapter's <=250ms software-observation bound.
        dt = signed_delta(source_ms, ticks)
        if dt < -10000 or dt > 60250:
            return refuse("source_age")
        drift_ms = abs(dt) / 2000 + 2  # 500 ppm clock drift plus timer quantisation.
        unadjusted = local_begin + dt
        if unadjusted > now_ms + 250 + (self.anchor_end - local_begin) + drift_ms:
            return refuse("future_source_time")
        # Round source time down and arrival time up to integer microseconds.
        # This avoids floating-point equality making an exactly expired record
        # appear a fraction of a microsecond younger than its 10-second limit.
        source_us = math.floor((unadjusted - 250 - drift_ms) * 1000)
        age_us = math.ceil(now_ms * 1000) - source_us
        if not 0 <= age_us < 10000000:
            return refuse("source_age")
        payload = data[12:12 + dlc]
        self.latest.pop(can_id, None)
        if dlc < MINIMUM_LENGTH[can_id]:
            return refuse("id_or_dlc")
        values = decode_can(can_id, payload)
        if values is None:
            return refuse("invalid_can_values")
        self.last_sequence = sequence
        self.accepted += 1
        name = f"0x{can_id:03x}"
        self.ids[name] = self.ids.get(name, 0) + 1
        self.latest[can_id] = (source_us, values)
        return True

    def snapshot(self, now_ms):
        fields = {}
        ages = {}
        now_us = math.ceil(now_ms * 1000)
        clock_verified = bool(self.anchor and 0 <= now_us - math.floor(self.anchor_end * 1000) < 60000000)
        for can_id, (sample_us, values) in self.latest.items():
            age_us = now_us - sample_us
            ages[f"0x{can_id:03x}"] = round(age_us / 1000000, 3)
            if clock_verified and 0 <= age_us < 10000000:
                fields.update(values)
        return {"accepted": self.accepted, "rejected": self.rejected,
                "last_error": self.last_error, "can_id_counts": self.ids,
                "ages_s": ages, "fresh_values": fields,
                "clock_verified": clock_verified,
                "source_rx_count": getattr(self, "rx", None),
                "adapter_drop_count": getattr(self, "dropped", None)}


def decode_can(can_id, p):
    def u16(pos):
        return struct.unpack_from("<H", p, pos)[0]
    if can_id == 0x351:
        volts = u16(0) / 10
        charge, discharge = struct.unpack_from("<hh", p, 2)
        if not (3 <= volts <= 160 and 0 <= charge <= 20000 and 0 <= discharge <= 20000):
            return None
        return dict(charge_voltage_limit_v=volts, charge_current_limit_a=charge / 10,
                    discharge_current_limit_a=discharge / 10)
    if can_id == 0x355:
        if not 0 <= u16(0) <= 100:
            return None
        values = dict(soc_percent=u16(0))
        if 0 <= u16(2) <= 100:
            values["soh_percent"] = u16(2)
        return values
    if can_id == 0x356:
        voltage, current, temperature = u16(0) / 100, struct.unpack_from("<h", p, 2)[0] / 10, struct.unpack_from("<h", p, 4)[0] / 10
        if not 3 <= voltage <= 160 or not -2000 <= current <= 2000:
            return None
        values = dict(voltage_v=voltage, current_a=current)
        if -100 <= temperature <= 150:
            values["average_temperature_c"] = temperature
        return values
    if can_id == 0x373:
        lo, hi = u16(0) / 1000, u16(2) / 1000
        if not 0.5 <= lo <= hi <= 5:
            return None
        values = dict(minimum_cell_v=lo, maximum_cell_v=hi, delta_cell_v=round(hi - lo, 3))
        tlo, thi = u16(4) - 273.15, u16(6) - 273.15
        if -100 <= tlo <= thi <= 150:
            values.update(minimum_cell_temperature_c=round(tlo, 2), maximum_cell_temperature_c=round(thi, 2))
        return values
    if can_id == 0x35C:
        if p[0] & 7:
            return None
        return dict(charge_enabled=bool(p[0] & 0x80), discharge_enabled=bool(p[0] & 0x40),
                    force_charge_i=bool(p[0] & 0x20), force_charge_ii=bool(p[0] & 0x10),
                    request_full_charge=bool(p[0] & 0x08))
    if can_id == 0x359:
        if not 1 <= p[4] <= 16:
            return None
        return dict(protection_bits=p[0] | p[1] << 8,
                    warning_bits=p[2] | p[3] << 8, module_count=p[4])
    if can_id == 0x35E:
        if any(c < 0x20 or c > 0x7E for c in p):
            return None
        return dict(protocol_name="PYLON" if p.rstrip(b" \x00") == b"PYLON" else "other (redacted)")
    return {}


def run_probe(seconds, expected_address=None):
    import dbus
    from dbus.mainloop.glib import DBusGMainLoop
    from gi.repository import GLib
    DBusGMainLoop(set_as_default=True)
    bus = dbus.SystemBus()
    manager = dbus.Interface(bus.get_object("org.bluez", "/"), "org.freedesktop.DBus.ObjectManager")
    context = GLib.MainContext.default()
    receiver = Receiver()
    report = {"probe": "pace_can_ble_read_only", "status": "starting",
              "cleanup": {"scan_stopped": True, "notifications_stopped": True, "disconnected": True}}
    start = time.monotonic()
    stop = start + seconds + 25
    adapter = None
    own_scan = False
    target = None
    frame_path = None
    own_notify = False
    connect_attempted = False
    match = None

    def interrupted(signum, frame):
        raise TimeoutError("interrupted")
    signal.signal(signal.SIGINT, interrupted)
    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGALRM, interrupted)
    signal.alarm(int(seconds + 35))

    def pump(duration, predicate=None):
        until = min(stop, time.monotonic() + duration)
        while time.monotonic() < until:
            while context.pending():
                context.iteration(False)
            if predicate and predicate():
                return True
            time.sleep(0.01)
        return bool(predicate and predicate())

    def properties(path, interface):
        return dbus.Interface(bus.get_object("org.bluez", path), "org.freedesktop.DBus.Properties").GetAll(interface, timeout=2)

    def refresh_metadata(path):
        begin_ms = time.monotonic() * 1000
        value = bytes(dbus.Interface(bus.get_object("org.bluez", path), "org.bluez.GattCharacteristic1")
                      .ReadValue(dbus.Dictionary({}, signature="sv"), timeout=2))
        end_ms = time.monotonic() * 1000
        if not receiver.metadata(value, begin_ms, end_ms):
            report["metadata_failures"] = report.get("metadata_failures", 0) + 1
            return False
        report["metadata_read_rtt_ms"] = round(end_ms - begin_ms, 2)
        return True

    def on_value(interface, changed, invalidated, path=None):
        if interface == "org.bluez.GattCharacteristic1" and str(path) == frame_path and "Value" in changed:
            receiver.receive(bytes(changed["Value"]), time.monotonic() * 1000)

    try:
        objects = manager.GetManagedObjects(timeout=2)
        adapters = [str(p) for p, interfaces in objects.items()
                    if "org.bluez.Adapter1" in interfaces and interfaces["org.bluez.Adapter1"].get("Powered", False)]
        if len(adapters) != 1:
            report["status"] = "need_one_powered_adapter"
            return report
        adapter_path = adapters[0]
        adapter = dbus.Interface(bus.get_object("org.bluez", adapter_path), "org.bluez.Adapter1")
        adapter.SetDiscoveryFilter(dbus.Dictionary({"Transport": dbus.String("le")}, signature="sv"), timeout=2)
        adapter.StartDiscovery(timeout=2)
        own_scan = True
        report["cleanup"]["scan_stopped"] = False
        pump(12)
        objects = manager.GetManagedObjects(timeout=2)
        adapter.StopDiscovery(timeout=2)
        own_scan = False
        report["cleanup"]["scan_stopped"] = True
        candidates = []
        for path, interfaces in objects.items():
            d = interfaces.get("org.bluez.Device1", {})
            if not d or str(d.get("Adapter", "")) != adapter_path:
                continue
            address = str(d.get("Address", "")).upper()
            uuids = {str(v).lower() for v in d.get("UUIDs", [])}
            name = str(d.get("Name", ""))
            qualifies = address == expected_address if expected_address else (SERVICE_UUID in uuids or name.startswith("brain_zero PACE"))
            if qualifies:
                candidates.append((str(path), d))
        report["candidate_count"] = len(candidates)
        if len(candidates) != 1:
            report["status"] = "need_one_adapter_candidate"
            return report
        path, device = candidates[0]
        target = path
        if device.get("Connected", False):
            report["status"] = "already_connected_refused"
            return report
        report["stage"] = "connect"
        connect_attempted = True
        report["cleanup"]["disconnected"] = False
        dbus.Interface(bus.get_object("org.bluez", target), "org.bluez.Device1").Connect(timeout=6)
        if not pump(6, lambda: bool(properties(target, "org.bluez.Device1").get("ServicesResolved", False))):
            report["status"] = "gatt_resolution_timeout"
            return report
        objects = manager.GetManagedObjects(timeout=2)
        services = {str(p) for p, i in objects.items() if "org.bluez.GattService1" in i
                    and str(i["org.bluez.GattService1"].get("Device", "")) == target
                    and str(i["org.bluez.GattService1"].get("UUID", "")).lower() == SERVICE_UUID}
        chars = {}
        for p, i in objects.items():
            c = i.get("org.bluez.GattCharacteristic1", {})
            if str(c.get("Service", "")) in services:
                chars[str(c.get("UUID", "")).lower()] = (str(p), {str(f) for f in c.get("Flags", [])}, c)
        if METADATA_UUID not in chars or FRAMES_UUID not in chars:
            report["status"] = "missing_contract_characteristic"
            return report
        metadata_path, mf, _ = chars[METADATA_UUID]
        frame_path, ff, fc = chars[FRAMES_UUID]
        if ("read" not in mf or "notify" not in ff or fc.get("Notifying", False)
                or any(f in mf | ff for f in ("write", "write-without-response"))):
            report["status"] = "unsafe_or_in_use_characteristics"
            return report
        report["characteristics"] = {"metadata": sorted(mf), "frames": sorted(ff)}
        report["stage"] = "metadata"
        if not refresh_metadata(metadata_path):
            report["status"] = "invalid_metadata"
            return report
        match = bus.add_signal_receiver(on_value, signal_name="PropertiesChanged",
                                        dbus_interface="org.freedesktop.DBus.Properties", path_keyword="path")
        dbus.Interface(bus.get_object("org.bluez", frame_path), "org.bluez.GattCharacteristic1").StartNotify(timeout=2)
        own_notify = True
        report["cleanup"]["notifications_stopped"] = False
        report["stage"] = "monitor"
        finish = min(stop, time.monotonic() + seconds)
        while time.monotonic() < finish:
            pump(min(10, finish - time.monotonic()))
            if time.monotonic() < finish:
                refresh_metadata(metadata_path)
        report.update(receiver.snapshot(time.monotonic() * 1000))
        report["status"] = "telemetry_received" if receiver.accepted else "no_valid_telemetry"
        return report
    except dbus.DBusException as exc:
        report["status"] = "dbus_error"
        report["error_name"] = exc.get_dbus_name()  # Message may contain addresses: omit it.
        return report
    except (TimeoutError, KeyboardInterrupt):
        report["status"] = "interrupted"
        return report
    finally:
        if match:
            match.remove()
        if own_notify:
            try:
                dbus.Interface(bus.get_object("org.bluez", frame_path), "org.bluez.GattCharacteristic1").StopNotify(timeout=1)
                report["cleanup"]["notifications_stopped"] = True
            except dbus.DBusException:
                pass
        if connect_attempted and target:
            try:
                dbus.Interface(bus.get_object("org.bluez", target), "org.bluez.Device1").Disconnect(timeout=2)
            except dbus.DBusException:
                pass
            try:
                report["cleanup"]["disconnected"] = pump(2, lambda: not bool(properties(target, "org.bluez.Device1").get("Connected", False)))
            except dbus.DBusException as exc:
                report["cleanup"]["disconnected"] = exc.get_dbus_name() in ("org.freedesktop.DBus.Error.UnknownObject", "org.bluez.Error.DoesNotExist")
        if own_scan and adapter:
            try:
                adapter.StopDiscovery(timeout=1)
                report["cleanup"]["scan_stopped"] = True
            except dbus.DBusException:
                pass
        report["elapsed_s"] = round(time.monotonic() - start, 2)
        signal.alarm(0)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seconds", type=int, default=30, help="Monitoring time after discovery (5..300)")
    parser.add_argument("--address", help="Optional configured bridge address; never output")
    parser.add_argument("--output", help="Sanitized JSON output path")
    args = parser.parse_args()
    if not 5 <= args.seconds <= 300:
        parser.error("seconds must be 5..300")
    address = args.address.upper() if args.address else None
    if address:
        import re
        if not re.fullmatch(r"(?:[0-9A-F]{2}:){5}[0-9A-F]{2}", address):
            parser.error("address syntax is invalid")
    result = run_probe(args.seconds, address)
    output = json.dumps(result, indent=2, sort_keys=True)
    if args.output:
        from pathlib import Path
        Path(args.output).write_text(output + "\n")
    print(output)
    return 0 if result["status"] == "telemetry_received" else 2


if __name__ == "__main__":
    sys.exit(main())
