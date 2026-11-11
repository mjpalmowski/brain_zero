"""Shared cached reads and confirmed, serialized controller operations."""
import asyncio
from copy import deepcopy
from datetime import timedelta
import logging
import math
from time import monotonic

from homeassistant.exceptions import HomeAssistantError
from homeassistant.helpers.update_coordinator import DataUpdateCoordinator, UpdateFailed

from .api import ApiError, SiteFeatureUnavailable
from .models import (BUS_ACTIONS, CHARGER_FIELDS, CHARGER_SCALES, DAYS, DOMAIN,
                     INVERTER_MODES, LIMITS, MODES, RECHARGE_FIELDS, flag,
                     live_reading, number, path, seconds, validate_recharge,
                     validate_schedule)

_LOGGER = logging.getLogger(__name__)
PERIODS = {"ess": 30, "inverter": 15, "recharge": 15, "free-power": 30}


class BrainCoordinator(DataUpdateCoordinator):
    def __init__(self, hass, entry, api):
        super().__init__(hass, _LOGGER, config_entry=entry, name=DOMAIN,
                         update_interval=timedelta(seconds=5), always_update=True)
        self.entry, self.api = entry, api
        self.snapshot = {}
        self.fetched = {}
        self.missing_site_sections = set()
        self.command_lock = asyncio.Lock()
        self.command_due = 0.0
        self.last_command = "No command sent"
        self.presets = dict(entry.options.get("limit_presets", {}))
        self.draft = dict(entry.options.get("free_power_draft", {}))
        self.draft_base = entry.options.get("free_power_base")
        self.identity = entry.unique_id
        self.charging = None

    async def fetch(self, endpoint, *, allow_missing=False):
        try:
            if self.charging and self.charging.active and endpoint in ("recharge", "free-power"):
                data = self.charging.view(endpoint)
            else:
                data = await self.api.get(endpoint)
        except SiteFeatureUnavailable:
            self.snapshot.pop(endpoint, None)
            self.fetched[endpoint] = monotonic()
            self.missing_site_sections.add(endpoint)
            if allow_missing:
                return None
            raise
        self.missing_site_sections.discard(endpoint)
        if endpoint == "state":
            ident = data.get("mqtt", {}).get("victron_gx", {}).get("installation_id")
            if self.identity and ident != self.identity:
                raise ApiError("The address now belongs to a different controller")
        self.snapshot[endpoint] = data
        self.fetched[endpoint] = monotonic()
        if self.charging:
            await self.charging.capture_legacy(endpoint, data)
        if endpoint == "ess":
            for key in LIMITS:
                value = data.get("settings", {}).get(key, -1)
                if value >= 0:
                    self.presets[key] = value
        if endpoint == "free-power" and not self.draft:
            self.draft = {k: data.get(k, 0) for k in ("start", "end", "charge_w", "chargers")}
            self.draft_base = data.get("id")
        return data

    async def _async_update_data(self):
        if self.command_lock.locked():
            return dict(self.snapshot)
        try:
            await self.fetch("state")
            if not all(k in self.fetched for k in PERIODS):
                for key in PERIODS:
                    await self.fetch(key, allow_missing=True)
            else:
                # One settings/status request per tick, shared by every entity.
                now = monotonic()
                due = [(now - self.fetched[k] - (300 if k in self.missing_site_sections else period), k)
                       for k, period in PERIODS.items()]
                overdue, key = max(due)
                if overdue >= 0:
                    try:
                        await self.fetch(key, allow_missing=True)
                    except ApiError as err:
                        _LOGGER.debug("Delayed %s refresh: %s", key, err)
            return dict(self.snapshot)
        except ApiError as err:
            raise UpdateFailed(str(err)) from err

    def publish(self):
        self.async_set_updated_data(dict(self.snapshot))

    def publish_auxiliary(self):
        """Notify local plan changes without postponing the controller poll.

        async_set_updated_data resets HA's refresh timer and marks the entire
        coordinator successful. The 2 s adapter must do neither to the 5 s
        controller polling loop.
        """
        self.data = dict(self.snapshot)
        self.async_update_listeners()

    def cached_snapshot(self):
        """Dashboard reads use this cache, never a second controller poll loop."""
        now = monotonic()
        data = deepcopy(self.snapshot)
        data.get("state", {}).pop("mqtt", None)
        data.get("state", {}).pop("ip", None)
        ages = {k: max(0, now - seen) for k, seen in self.fetched.items()}
        for section in ("charger", "assist"):
            if section in data:
                data[section]["stored_readback"] = True
                for p in data[section].get("parameters", []):
                    if p.get("age_s", -1) >= 0:
                        p["age_s"] += ages.get(section, 0)
        return {"controller": self.identity, "snapshot": data, "age_s": ages,
                "online": self.last_update_success and ages.get("state", 999) < 15,
                "charging_adapter": {"enabled": self.charging.enabled, "unified_firmware": self.charging.supported,
                                     "plans_imported": all(k in self.charging.data for k in ("recharge", "free-power")),
                                     "managed_by": "Home Assistant" if self.charging.active else "Controller"}
                if self.charging else {"enabled": False}}

    def save_options(self):
        self.hass.config_entries.async_update_entry(self.entry, options={**self.entry.options,
            "limit_presets": self.presets, "free_power_draft": self.draft, "free_power_base": self.draft_base})

    def value(self, spec):
        source, field = spec.source, spec.field
        if source in ("ac_input", "pv_disable"):
            data = self.snapshot.get("state", {}).get(source, {})
            return data.get("settings", {}).get(field) if spec.kind == "number" else data.get(field)
        if source == "local":
            return {"last_command": self.last_command, "requests": self.api.requests, "errors": self.api.errors}.get(field)
        if source == "draft":
            return self.draft.get(field)
        data = self.snapshot.get("ess" if source == "schedule" else source, {})
        if source == "state":
            elapsed = monotonic() - self.fetched.get("state", 0)
            if field == "dc_power":
                v, a = live_reading(data, "bat_v", elapsed), live_reading(data, "bat_a", elapsed)
                return None if v is None or a is None else round(v * a, 2)
            if field in ("bat_v", "bat_a", "soc", "mains_v", "mains_a", "acin_w", "acout_w", "inverter_v", "inverter_a", "ac_hz"):
                value = live_reading(data, field, elapsed)
                if spec.key == "ac_in_frequency":
                    volts = live_reading(data, "mains_v", elapsed)
                    if volts is None or volts <= 50:
                        return None
                return -value if field == "acin_w" and value is not None else value
            return path(data, field)
        if source == "ess":
            value = data.get("settings", {}).get(field)
            if field in LIMITS:
                if value is None:
                    return None
                return value >= 0 if spec.kind == "switch" else value if value >= 0 else self.presets.get(field, 5000)
            return MODES[value] if field == "mode" and isinstance(value, int) and 0 <= value < 4 else value
        if source == "schedule":
            rows = data.get("settings", {}).get("schedules", [])
            if spec.slot >= len(rows):
                return None
            value = rows[spec.slot].get(field)
            if field == "day" and isinstance(value, int) and 0 <= value <= 10:
                return DAYS[value]
            return value / 60 if field == "duration" and value is not None else value
        if source in ("charger", "assist"):
            return next((p.get("value") for p in data.get("parameters", []) if p.get("key") == field and p.get("supported")), None)
        if source == "recharge":
            if spec.group == "battery" and not data.get("battery_valid"):
                return None
            if field == "day":
                day = data.get(field)
                return DAYS[day] if isinstance(day, int) and 0 <= day <= 10 else None
        return path(data, field)

    def available(self, spec):
        if not self.last_update_success or monotonic() - self.fetched.get("state", 0) > 15:
            return False
        source = "ess" if spec.source == "schedule" else spec.source
        if source in self.missing_site_sections or source == "draft" and "free-power" in self.missing_site_sections:
            return False
        if source in PERIODS and monotonic() - self.fetched.get(source, 0) > 120:
            return False
        if spec.source == "inverter" and spec.kind != "sensor":
            data = self.snapshot.get("inverter", {})
            if not data.get("available"):
                return False
            if spec.key == "inverter_mode":
                return bool(data.get("mode_adjustable"))
            if spec.key == "input_limit":
                return bool(data.get("current_adjustable"))
            if spec.key == "equalize":
                return bool(data.get("can_equalize"))
            if spec.key in ("stop_equalize_absorption", "stop_equalize_float"):
                return bool(data.get("can_interrupt"))
        return spec.kind == "button" or self.value(spec) is not None

    async def execute(self, label, operation):
        # Reject an overlapping action rather than accumulating stale commands.
        if self.command_lock.locked():
            raise HomeAssistantError("Another brain_zero command is still being verified")
        async with self.command_lock:
            await asyncio.sleep(max(0, self.command_due - monotonic()))
            self.command_due = monotonic() + 2
            self.last_command = "Verifying: " + label
            self.publish()
            try:
                async with asyncio.timeout(45):
                    result = await operation()
                    # Publishing resets HA's poll timer. Refresh telemetry first so
                    # a slow command cannot republish expired electrical readings.
                    await self.fetch("state")
            except (ApiError, ValueError, KeyError, TimeoutError) as err:
                self.last_command = ("Not confirmed: " + str(err))[:240]
                self.publish()
                raise HomeAssistantError(self.last_command) from err
            self.last_command = ("Confirmed: " + label)[:240]
            self.publish()
            return result

    async def set_ess(self, fields):
        allowed = {"run", "mode", "min_soc", "grid_setpoint", *LIMITS, "always_peak_shave", "dc_pv_feed_in",
                   "ext_no_charge", "ext_no_feed_in", "safety_max"}
        if not fields or set(fields) - allowed:
            raise ValueError("Unsupported ESS setting")
        checked = {}
        for key, value in fields.items():
            if key in {"run", "always_peak_shave", "dc_pv_feed_in", "ext_no_charge", "ext_no_feed_in"}:
                checked[key] = flag(value)
            elif key == "mode":
                checked[key] = int(number(value, 0, 3))
            else:
                checked[key] = number(value, -5000 if key == "grid_setpoint" else -1 if key in LIMITS else 0,
                                      100 if key == "min_soc" else 5000, .1 if key == "min_soc" else 1)
        before = (await self.fetch("ess"))["settings"]
        if any(before.get(k) != v for k, v in checked.items()):
            await self.api.post("ess", checked)
        after = (await self.fetch("ess"))["settings"]
        if any(after.get(k) != v for k, v in checked.items()):
            raise ApiError("ESS settings did not match saved readback")
        if set(checked).intersection(LIMITS):
            self.save_options()

    async def set_limit(self, spec, value):
        before = (await self.fetch("ess"))["settings"]
        if spec.kind == "switch":
            flag(value)
            if before[spec.field] >= 0:
                self.presets[spec.field] = before[spec.field]
            target = self.presets.get(spec.field, 5000) if value else -1
            await self.set_ess({spec.field: target})
        else:
            target = number(value, 0, 5000)
            self.presets[spec.field] = target
            if before[spec.field] >= 0:
                await self.set_ess({spec.field: target})
        self.save_options()

    async def set_schedule(self, slot, fields):
        slot = int(number(slot, 1, 5)) - 1
        if not fields or set(fields) - {"on", "day", "start", "duration", "soc", "discharge"}:
            raise ValueError("Unsupported schedule field")
        current = (await self.fetch("ess"))["settings"]["schedules"][slot]
        requested = validate_schedule({**current, **fields})
        if requested != current:
            await self.api.post("ess", {f"s{slot+1}_{k}": requested[k] for k in fields if current.get(k) != requested[k]})
        actual = (await self.fetch("ess"))["settings"]["schedules"][slot]
        if any(actual.get(k) != requested[k] for k in fields):
            raise ApiError("Schedule did not match saved readback")

    async def set_schedules(self, windows, base):
        if not isinstance(windows, list) or len(windows) != 5 or not isinstance(base, list) or len(base) != 5:
            raise ValueError("Supply five schedules and their original readback")
        keys = {"on", "day", "start", "duration", "soc", "discharge"}
        if any(not isinstance(row, dict) or set(row) != keys for row in windows + base):
            raise ValueError("Each schedule must contain exactly the six supported fields")
        desired = [validate_schedule(row) for row in windows]
        original = [validate_schedule(row) for row in base]
        before = (await self.fetch("ess"))["settings"]["schedules"]
        if before != original:
            raise ApiError("The saved schedules changed while editing. Reload before saving")
        changes = {f"s{i+1}_{key}": value for i, row in enumerate(desired)
                   for key, value in row.items() if before[i].get(key) != value}
        if changes:
            await self.api.post("ess", changes)
        if (await self.fetch("ess"))["settings"]["schedules"] != desired:
            raise ApiError("Schedules did not match saved readback")

    async def set_overnight(self, fields, expected_id=None):
        if not fields or set(fields) - set(RECHARGE_FIELDS):
            raise ValueError("Unsupported overnight setting")
        if self.charging and self.charging.active:
            await self.charging.set_overnight(fields, expected_id)
            await self.fetch("recharge")
            return
        before = await self.fetch("recharge")
        if expected_id is not None and number(expected_id, 0, 4294967295) != before.get("id"):
            raise ApiError("The overnight schedule changed while editing. Reload before saving")
        desired = validate_recharge({**before, **fields})
        if any(before.get(k) != v for k, v in desired.items()):
            await self.api.post("recharge", {"base_id": before["id"], **desired})
        for _ in range(12):
            actual = await self.fetch("recharge")
            if any(actual.get(k) != v for k, v in desired.items()):
                raise ApiError("Overnight settings did not match S3 readback")
            peer = actual.get("peer", {})
            if peer.get("reachable") and peer.get("matching") and peer.get("clock_valid"):
                return
            await asyncio.sleep(1)
        raise ApiError("Saved on S3; R4875 confirmation is pending. The write will not be repeated")

    async def set_inverter(self, key, value):
        view = await self.fetch("inverter")
        if not view.get("available"):
            raise ApiError(view.get("reason", "The inverter is not ready for commands"))
        if key == "inverter_mode":
            if value not in INVERTER_MODES or not view.get("mode_adjustable"):
                raise ValueError("Operating mode is not adjustable")
            if view.get("mode_name") == value:
                return
            expected = INVERTER_MODES.index(value) + 4
        elif key == "ac_input_limit":
            value = number(value, 0, 50, .1)
            if not view.get("current_adjustable") or value > view.get("maximum_a", -1) or (value and value < view.get("minimum_a", 0)):
                raise ValueError("Current is outside the inverter's adjustable range")
            if view.get("current_a") == value:
                return
            expected = int(round(value * 10))
        elif key in BUS_ACTIONS:
            if not view.get("available") or (key == "equalize" and not view.get("can_equalize")) or (
                    key.startswith("stop_equalize") and not view.get("can_interrupt")):
                raise ValueError(view.get("reason", "The inverter is not ready for this command"))
            value, expected = "PRESS", 0
        else:
            raise ValueError("Unsupported inverter command")
        await self.api.post("inverter", {key: value})
        actions = {"inverter_mode": 1, "ac_input_limit": 2, "equalize": 3, "stop_equalize_absorption": 4,
                   "stop_equalize_float": 5, "restart_vebus": 6, "redetect_vebus": 7}
        for _ in range(20):
            await asyncio.sleep(1)
            after = await self.fetch("inverter")
            job = after.get("command", {})
            if job.get("action") != actions[key] or job.get("value") != expected:
                raise ApiError("A different inverter command replaced this one; check live readback")
            if job.get("state") == "failed":
                raise ApiError(job.get("message", "Inverter refused the command"))
            if job.get("state") == "confirmed":
                if key == "inverter_mode" and after.get("mode_name") != value:
                    raise ApiError("Operating mode did not match inverter readback")
                if key == "ac_input_limit" and after.get("current_a") != value:
                    raise ApiError("Input current limit did not match inverter readback")
                return
        raise ApiError("Inverter confirmation timed out; command was not repeated")

    async def wait_charger(self):
        for _ in range(20):
            data = await self.fetch("charger")
            if data.get("state") not in ("reading", "writing", "verifying"):
                if data.get("state") != "done":
                    raise ApiError(data.get("message", "Charger read failed"))
                return data
            await asyncio.sleep(1)
        raise ApiError("Charger readback timed out; no repeated writes were sent")

    async def refresh_charger(self):
        # GET itself can start a firmware read once its cache has expired.
        data = await self.fetch("charger")
        if data.get("state") not in ("reading", "writing", "verifying"):
            await self.api.post("charger/read", {})
        return await self.wait_charger()

    async def set_charger(self, fields, base=None):
        if not fields or set(fields) - set(CHARGER_FIELDS):
            raise ValueError("Unsupported charger setting")
        before = await self.refresh_charger()
        params = {p["key"]: p for p in before.get("parameters", [])}
        if base is not None and base != ",".join(str(params.get(k, {}).get("raw")) for k in CHARGER_FIELDS):
            raise ApiError("The stored charger settings changed while editing. Read from the inverter before saving")
        desired, expected = {}, []
        for key, scale in zip(CHARGER_FIELDS, CHARGER_SCALES, strict=True):
            p = params.get(key, {})
            if not p.get("supported") or p.get("value") is None or not 0 <= p.get("age_s", -1) < 120:
                raise ApiError("Fresh supported charger metadata is required")
            hi = min(70 if key == "charge_current" else 64, p["maximum"])
            lo = max(0 if key == "charge_current" else 48, p["minimum"])
            desired[key] = number(fields.get(key, p["value"]), lo, hi, 1 / scale)
            expected.append(p["raw"])
        if desired["float_voltage"] > desired["absorption_voltage"]:
            raise ValueError("Float voltage must not exceed absorption voltage")
        if all(params[k]["value"] == v for k, v in desired.items()):
            return
        await self.api.post("charger", {**desired, "base": ",".join(str(v) for v in expected)})
        after = await self.wait_charger()
        actual = {p["key"]: p for p in after.get("parameters", [])}
        if not after.get("saving") or any(not actual.get(k, {}).get("verified") or
                actual[k].get("raw") != round(v * CHARGER_SCALES[CHARGER_FIELDS.index(k)]) for k, v in desired.items()):
            raise ApiError("Stored charger settings did not pass inverter readback")

    async def free_power(self, cancel=False, fields=None):
        if self.charging and self.charging.active:
            await self.charging.set_free(cancel, fields)
            await self.fetch("free-power")
            return
        before = await self.fetch("free-power")
        if cancel:
            if not before.get("enabled"):
                return
            await self.api.post("free-power", {"base_id": before["id"], "cancel": 1})
        else:
            if fields is None and self.draft_base != before.get("id"):
                raise ApiError("The saved session changed. Reload the free-power draft before arming")
            desired = {**self.draft, **(fields or {})}
            if set(desired) - {"start", "end", "charge_w", "chargers"}:
                raise ValueError("Unsupported free-power setting")
            desired = {"start": int(number(desired["start"], 1767225600, 4294967295)),
                       "end": int(number(desired["end"], 1767225600, 4294967295)),
                       "charge_w": int(number(desired["charge_w"], 0, 5000)), "chargers": flag(desired["chargers"])}
            if desired["start"] >= desired["end"] or desired["end"] - desired["start"] > 86400:
                raise ValueError("Use a dated window lasting up to 24 hours")
            await self.api.post("free-power", {"base_id": before["id"], **desired})
        for _ in range(12):
            after = await self.fetch("free-power")
            if cancel:
                if after.get("enabled"):
                    raise ApiError("Cancellation did not match S3 readback")
            elif any(after.get(k) != v for k, v in desired.items()) or not after.get("enabled"):
                raise ApiError("Free-power session did not match S3 readback")
            peer = after.get("peer", {})
            if not after.get("chargers") or (peer.get("matching") and peer.get("reachable") and peer.get("clock_valid")):
                self.draft = {k: after[k] for k in ("start", "end", "charge_w", "chargers")}
                self.draft_base = after["id"]
                self.save_options()
                return
            await asyncio.sleep(1)
        raise ApiError("Saved on S3; R4875 confirmation is pending. The write will not be repeated")

    async def refresh_assist(self):
        await self.api.post("assist/read", {})
        for _ in range(20):
            view = await self.fetch("assist")
            if view.get("state") not in ("reading", "writing", "verifying"):
                if view.get("state") != "done":
                    raise ApiError(view.get("message", "PowerAssist settings read failed"))
                return view
            await asyncio.sleep(1)
        raise ApiError("PowerAssist readback timed out; no repeated writes were sent")

    async def restart_controller(self):
        before = await self.api.get("system")
        if before.get("controller") != self.identity or not isinstance(before.get("boot_id"), int):
            raise ApiError("Controller identity could not be verified before restart")
        await self.api.post("system", {"reboot": "PRESS"})
        for _ in range(35):
            await asyncio.sleep(1)
            try:
                after = await self.api.get("system")
            except ApiError:
                continue
            if after.get("controller") != self.identity:
                raise ApiError("A different controller answered after restart")
            if isinstance(after.get("boot_id"), int) and after["boot_id"] != before["boot_id"]:
                await self.fetch("state")
                return
            if not after.get("restart_pending"):
                raise ApiError(after.get("message", "Controller restart was not confirmed"))
        raise ApiError("Controller restart was not confirmed; request was not repeated")

    async def set_assist(self, fields, base=None):
        if not fields or set(fields) - {"power_assist", "boost_factor"}:
            raise ValueError("Unsupported PowerAssist setting")
        before = await self.refresh_assist()
        p = {v["key"]: v for v in before.get("parameters", [])}
        keys = ("power_assist", "boost_factor")
        if any(not p.get(k, {}).get("supported") or p[k].get("value") is None or not 0 <= p[k].get("age_s", -1) < 120 for k in keys):
            raise ApiError("Fresh supported PowerAssist readbacks are required")
        raw_base = ",".join(str(p[k]["raw"]) for k in keys)
        if base is not None and base != raw_base:
            raise ApiError("PowerAssist settings changed while editing. Read before saving")
        desired = {"power_assist": flag(fields.get("power_assist", p["power_assist"]["value"])),
                   "boost_factor": number(fields.get("boost_factor", p["boost_factor"]["value"]),
                       max(.25, p["boost_factor"]["minimum"]), min(3.5, p["boost_factor"]["maximum"]), .0625)}
        if all(p[k]["value"] == v for k, v in desired.items()):
            return
        await self.api.post("assist", {**desired, "base": raw_base})
        for _ in range(20):
            after = await self.fetch("assist")
            if after.get("state") not in ("reading", "writing", "verifying"):
                actual = {v["key"]: v for v in after.get("parameters", [])}
                expected = {"power_assist": (p["power_assist"]["raw"] & ~32) | (32 if desired["power_assist"] else 0),
                            "boost_factor": round(desired["boost_factor"] * 16)}
                if after.get("state") != "done" or not after.get("saving") or any(
                        not actual.get(k, {}).get("verified") or actual[k].get("raw") != value for k, value in expected.items()):
                    raise ApiError(after.get("message", "PowerAssist settings did not pass inverter readback"))
                return
            await asyncio.sleep(1)
        raise ApiError("PowerAssist save confirmation timed out; command was not repeated")

    async def set_ac_input(self, fields, base=None):
        if not fields or set(fields) - {"ignore", "reconnect_soc", "reconnect_v", "minutes"}:
            raise ValueError("Unsupported AC-input control")
        before = (await self.fetch("state")).get("ac_input", {})
        revision = before.get("revision")
        if not isinstance(revision, int) or (base is not None and number(base, 0, 4294967295) != revision):
            raise ApiError("AC-input state changed. Reload before applying")
        # Configuration and actuation are distinct transactions. A failed save cannot enable islanding.
        settings = dict(before.get("settings", {}))
        config_fields = set(fields) - {"ignore"}
        if config_fields:
            desired = {**settings, **{k: fields[k] for k in config_fields}}
            desired = {"reconnect_soc": number(desired["reconnect_soc"], 0, 95),
                       "reconnect_v": number(desired["reconnect_v"], 48, 56, .1),
                       "minutes": int(number(desired["minutes"], 1, 240))}
            if desired != settings:
                await self.api.post("ac-input/settings", {**desired, "base": revision})
                after = (await self.fetch("state")).get("ac_input", {})
                if after.get("settings") != desired:
                    raise ApiError("Reconnect protection settings did not match saved readback")
                before, revision = after, after["revision"]
        if "ignore" not in fields:
            return
        target = bool(flag(fields["ignore"]))
        if target and not before.get("requested") and not before.get("can_enable"):
            raise ApiError("Ignore AC is blocked by local protection or pending grid readback")
        if target != before.get("requested"):
            await self.api.post("ac-input", {"ignore": int(target), "base": revision})
        for _ in range(32):
            actual = (await self.fetch("state")).get("ac_input", {})
            if target and not actual.get("requested"):
                raise ApiError(actual.get("reason", "Local protection cancelled Ignore AC"))
            if target and actual.get("ignored") is True and actual.get("phase") == "islanded":
                return
            if not target and actual.get("ignored") is False and actual.get("grid_connected") is True:
                return
            await asyncio.sleep(1)
        raise ApiError("AC-input readback still pending; command was not repeated. Check connection status")

    async def set_pv_disable(self, fields, base=None):
        if not fields or set(fields) - {"disable", "seconds"}:
            raise ValueError("Unsupported PV control")
        target = bool(flag(fields["disable"])) if "disable" in fields else None
        duration = int(number(fields["seconds"], 10, 300)) if "seconds" in fields else None
        before = (await self.fetch("state")).get("pv_disable", {})
        revision = before.get("revision")
        if not isinstance(revision, int) or (base is not None and number(base, 0, 4294967295) != revision):
            raise ApiError("PV control changed. Refresh before applying")
        if "seconds" in fields:
            if before.get("settings", {}).get("seconds") != duration:
                await self.api.post("pv-disable/settings", {"seconds": duration, "base": revision})
                before = (await self.fetch("state")).get("pv_disable", {})
                if before.get("settings", {}).get("seconds") != duration:
                    raise ApiError("PV restoration duration did not match saved readback")
                revision = before["revision"]
        if "disable" not in fields:
            return
        if target and not before.get("requested") and not before.get("can_enable"):
            raise ApiError(before.get("blocked") or "Wait for confirmed Ignore AC island operation and fresh PV readback")
        if not target or not before.get("requested"):
            await self.api.post("pv-disable", {"disable": int(target), "base": revision})
        for _ in range(12):
            actual = (await self.fetch("state")).get("pv_disable", {})
            if target and not actual.get("requested"):
                raise ApiError(actual.get("reason", "Local protection restored PV operation"))
            if actual.get("disabled") is target and actual.get("transport", {}).get("verified"):
                return
            await asyncio.sleep(1)
        raise ApiError("PV flag readback pending; command was not repeated. Check restoration status")

    async def apply(self, spec, value=None):
        async def action():
            source, field = spec.source, spec.field
            if source == "pv_disable":
                await self.set_pv_disable({"disable" if field == "requested" else field: value})
                return
            if source == "ac_input":
                await self.set_ac_input({"ignore" if field == "requested" else field: value})
                return
            if source == "draft":
                if field == "chargers":
                    value_checked = flag(value)
                else:
                    value_checked = int(number(value, 0, 5000)) if field == "charge_w" else int(number(value, 1767225600, 4294967295))
                self.draft[field] = value_checked
                self.save_options()
                return
            if source == "ess":
                if field in LIMITS:
                    await self.set_limit(spec, value)
                else:
                    await self.set_ess({field: MODES.index(value) if field == "mode" else value})
            elif source == "schedule":
                val = DAYS.index(value) if field == "day" else round(number(value, 0, 1440) * 60) if field == "duration" else value
                await self.set_schedule(spec.slot + 1, {field: val})
            elif source == "recharge":
                await self.set_overnight({field: DAYS.index(value) if field == "day" else value})
            elif source == "charger":
                await self.set_charger({field: value})
            elif source == "assist":
                await self.set_assist({field: value})
            elif source == "inverter":
                await self.set_inverter("ac_input_limit" if spec.key == "input_limit" else spec.key, value)
            elif spec.key == "refresh_charger":
                await self.refresh_charger()
            elif spec.key == "read_assist":
                await self.refresh_assist()
            elif spec.key == "restart_controller":
                await self.restart_controller()
            elif spec.key == "refresh_settings":
                for endpoint in PERIODS:
                    await self.fetch(endpoint)
            elif spec.key == "reload_free_power":
                current = await self.fetch("free-power")
                self.draft = {k: current[k] for k in ("start", "end", "charge_w", "chargers")}
                self.draft_base = current["id"]
                self.save_options()
            elif spec.key in ("arm_free_power", "cancel_free_power"):
                await self.free_power(cancel=spec.key == "cancel_free_power")
            else:
                raise ValueError("Unsupported brain_zero control")
        await self.execute(spec.name, action)
