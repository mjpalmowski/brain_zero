"""HA-owned charging plans and a bounded, generic controller interface.

Legacy controllers remain a read/write passthrough until the unified battery
endpoint is detected. Persist plans through Home Assistant Store, not raw files.
"""
import asyncio
from copy import deepcopy
from datetime import datetime, timedelta
import hashlib
import math
import re
from time import monotonic, time
from zoneinfo import ZoneInfo

from .api import ApiError
from .battery_link import read_battery
from .models import RECHARGE_FIELDS, flag, live_reading, number, validate_recharge

OPTIONS = ("charging_adapter", "battery_link_host", "battery_link_token", "r4875_host", "r4875_token")
RECHARGE_KEYS = (*RECHARGE_FIELDS, "id", "version", "timezone", "grid_after_target")
FREE_KEYS = ("version", "id", "start", "end", "charge_w", "enabled", "completed", "chargers")
LEASE_SECONDS = 20


def clean_plan(path, value, timezone):
    """Keep only validated configuration, never cached status or credentials."""
    keys = RECHARGE_KEYS if path == "recharge" else FREE_KEYS
    plan = {key: value[key] for key in keys if key in value}
    plan["id"] = int(number(plan.get("id", 0), 0, 4294967295))
    if path == "recharge":
        plan.update(validate_recharge(plan))
        plan["timezone"] = plan.get("timezone", timezone)
        ZoneInfo(plan["timezone"])
        plan["grid_after_target"] = flag(plan.get("grid_after_target", 0))
    else:
        for key in ("enabled", "completed", "chargers"):
            plan[key] = flag(plan.get(key, 0))
        plan["charge_w"] = int(number(plan.get("charge_w", 2500), 0, 5000))
        for key in ("start", "end"):
            plan[key] = int(number(plan.get(key, 0), 0, 4294967295))
        if plan["id"] and not (1767225600 <= plan["start"] < plan["end"] and plan["end"] - plan["start"] <= 86400):
            raise ValueError("Invalid saved free-power window")
        if not plan["id"] and plan["enabled"]:
            raise ValueError("An armed free-power window needs a revision")
    return plan


def day_matches(code, day):
    sunday = (day.weekday() + 1) % 7
    return (code == 7 or code == sunday or code == 8 and sunday in range(1, 6) or
            code == 9 and sunday in (0, 6) or code == 10 and day.day == 1)


def local_epoch(day, seconds, zone, end=False):
    naive = datetime.combine(day, datetime.min.time()) + timedelta(seconds=seconds)
    candidates = [naive.replace(tzinfo=zone, fold=fold).timestamp() for fold in (0, 1)]
    valid = [stamp for stamp in candidates if datetime.fromtimestamp(stamp, zone).replace(tzinfo=None) == naive]
    # Autumn: first start/last end. Missing spring clocks normalize forward.
    return (max(valid) if end else min(valid)) if valid else max(candidates)


def recurring_window(plan, now):
    if not plan.get("enabled"):
        return None
    zone = ZoneInfo(plan.get("timezone", "Europe/London"))
    today = datetime.fromtimestamp(now, zone).date()
    for day in (today, today - timedelta(days=1)):
        if not day_matches(plan["day"], day):
            continue
        start = local_epoch(day, plan["start"], zone)
        end_day = day + timedelta(days=plan["end"] < plan["start"])
        end = local_epoch(end_day, plan["end"], zone, True)
        if start <= now < end:
            return {"day": day.isoformat(), "start": start, "end": end}
    return None


def free_phase(plan, now, completed_id=0):
    if not plan.get("id"):
        return "empty"
    if plan.get("completed") or completed_id == plan["id"] or now >= plan["end"]:
        return "finished" if plan.get("enabled") or plan.get("completed") else "cancelled"
    if not plan.get("enabled"):
        return "cancelled"
    return "active" if now >= plan["start"] else "armed"


def schedule_row(plan, previous):
    return {"on": int(bool(plan["enabled"] and plan["victron"])), "day": plan["day"],
            "start": plan["start"], "duration": (plan["end"] - plan["start"]) % 86400,
            "soc": plan["victron_soc"], "discharge": previous.get("discharge", 1)}


def lease_decision(recurring, free, now, soc, global_cap, memo):
    cap = global_cap if global_cap >= 0 else 5000
    phase = free_phase(free, now, memo.get("completed_id", 0))
    if phase == "active":
        return "free:" + str(free["id"]), min(free["charge_w"], cap)
    window = recurring_window(recurring, now)
    # The ordinary native window handles the existing above-global-cap plan.
    # HA supplies only a temporary lower cap, preserving the native fallback.
    if window and recurring.get("victron") and recurring["victron_w"] < cap:
        key = f"overnight:{recurring['id']}:{window['day']}"
        if soc is None:
            return key, 0
        reached = memo.get("target_window") == key and memo.get("target_met", False)
        reached = soc >= recurring["victron_soc"] or reached and soc > recurring["victron_soc"] - 1
        memo.update(target_window=key, target_met=reached)
        if reached:
            return (key, 0) if recurring.get("grid_after_target") else (None, 0)
        return key, min(recurring["victron_w"], cap)
    return None, 0


def check_peer_revision(path, desired, current, expected_id):
    if expected_id is None or current.get("id") in (expected_id, desired["id"]):
        return
    # Migration can start with an empty HA session and an older completed
    # charger session. Only a fresh terminal read permits that first explicit
    # arm; an armed, active or newer revision remains protected.
    terminal_bootstrap = (path == "free-power" and expected_id == 0 and desired.get("enabled") and
                          0 < current.get("id", 0) < desired["id"] and current.get("clock_valid") and
                          not current.get("active") and
                          (current.get("completed") or 0 < current.get("end", 0) <= time()))
    if not terminal_bootstrap:
        raise ApiError("The external charger plan changed. Read it back before replacing it")


class ChargingAdapter:
    def __init__(self, coordinator, transport, *, store=None, battery_reader=read_battery):
        self.c = coordinator
        self.hass, self.entry, self.api = coordinator.hass, coordinator.entry, coordinator.api
        self.transport = transport
        self.options = dict(self.entry.options)
        self.enabled = bool(self.options.get("charging_adapter", False))
        self.store = store
        self.battery_reader = battery_reader
        self.data = {"memo": {}}
        self.supported = False
        self.probed = False
        self.probe_at = self.peer_at = 0
        self.battery = None
        self.peer = {"recharge": {}, "free-power": {}}
        self.peer_seen = {"recharge": 0, "free-power": 0}
        self.error = ""
        self.lock = asyncio.Lock()
        self.lease_id = None
        self.last_lease = 0
        self._stop = None
        self.stopped = False
        self.topic = None
        self.sent_timestamps = {}

    @property
    def active(self):
        return self.enabled and self.supported

    async def start(self):
        if not self.enabled:
            return
        if self.store is None:
            from homeassistant.helpers.storage import Store
            self.store = Store(self.hass, 1, "brain_zero_charging_" + self.entry.entry_id)
        self.restore(await self.store.async_load())
        from homeassistant.helpers.event import async_track_time_interval
        self._stop = async_track_time_interval(self.hass, self._tick_event, timedelta(seconds=2))
        self.entry.async_on_unload(self.stop)

    def restore(self, saved):
        if not isinstance(saved, dict):
            return
        self.data = deepcopy(saved)
        if not isinstance(self.data.get("memo"), dict):
            self.data["memo"] = {}
        for path in ("recharge", "free-power"):
            if path in self.data:
                self.data[path] = clean_plan(path, self.data[path], self.hass.config.time_zone)
        previous = self.data["memo"].get("lease_id")
        self.lease_id = previous if isinstance(previous, str) and re.fullmatch(r"[0-9a-f]{32}", previous) else None

    def stop(self):
        self.stopped = True
        if self._stop:
            self._stop()
            self._stop = None

    async def _tick_event(self, _now):
        if self.stopped or self.lock.locked():
            return
        async with self.lock:
            try:
                await self.tick()
                self.error = ""
            except (ApiError, OSError, TimeoutError, ValueError) as err:
                self.error = str(err)[:160]
            if self.active:
                for path in ("recharge", "free-power"):
                    self.c.snapshot[path] = self.view(path)
                    self.c.fetched[path] = monotonic()
                    self.c.missing_site_sections.discard(path)
                self.c.publish()

    async def save(self):
        if self.store:
            await self.store.async_save(deepcopy(self.data))

    async def capture_legacy(self, path, value):
        if not self.enabled or self.supported or path not in ("recharge", "free-power"):
            return
        plan = clean_plan(path, value, self.hass.config.time_zone)
        if self.data.get(path) != plan:
            self.data[path] = plan
            await self.save()

    async def probe(self):
        if monotonic() < self.probe_at:
            return
        self.probe_at = monotonic() + 30
        try:
            state = await self.api.get("battery-input")
        except ApiError as err:
            if getattr(err, "status", None) == 404:
                self.supported, self.probed = False, True
                return
            raise
        self.supported = isinstance(state.get("enabled"), bool)
        self.probed = True
        self.topic = state.get("topic")
        self.c.snapshot["battery-input"] = state
        self.c.fetched["battery-input"] = monotonic()
        if self.supported:
            self.probe_at = monotonic() + 5
        if self.supported and (not isinstance(self.topic, str) or not self.topic.endswith("/external_battery") or
                               len(self.topic) > 128 or any(c in self.topic for c in "+#\x00")):
            raise ValueError("Controller returned an invalid battery input topic")
        if self.supported and not state["enabled"]:
            enabled = await self.api.post("battery-input", {"enabled": True})
            if enabled.get("enabled") is not True:
                raise ApiError("The controller did not confirm battery input configuration")
            self.c.snapshot["battery-input"] = enabled

    async def tick(self):
        if not self.enabled:
            return
        await self.probe()
        if not self.active:
            return
        now = time()
        host, key = self.options.get("battery_link_host"), self.options.get("battery_link_token")
        if host and key:
            try:
                sample = await self.battery_reader(host, key)
                if not self.battery or sample.sequence != self.battery.sequence or not self.battery.payload(time()):
                    self.battery = sample
            except (OSError, TimeoutError, ValueError):
                # One lost datagram does not invalidate a previously verified
                # sample; its original timestamp still expires after 10 seconds.
                pass
        if monotonic() - self.peer_at >= 4 and self.options.get("r4875_host"):
            self.peer_at = monotonic()
            for path in ("recharge", "free-power"):
                try:
                    self.peer[path] = await self.peer_request(path)
                except (OSError, TimeoutError, ValueError):
                    self.peer_seen[path] = 0
        battery_payload = self.battery.payload(time()) if self.battery else {}
        payload = dict(battery_payload)
        peer = self.peer["recharge"]
        seen = self.peer_seen["recharge"]
        watts = peer.get("dc_w")
        volts = battery_payload.get("voltage")
        if volts is None:
            bus = self.c.snapshot.get("state", {}).get("bat_v", [])
            cache_age = monotonic() - self.c.fetched.get("state", 0)
            if len(bus) == 2 and isinstance(bus[0], (int, float)) and isinstance(bus[1], (int, float)) and 0 <= bus[1] + cache_age <= 6:
                volts = bus[0]
        if (time() - seen <= 6 and peer.get("clock_valid") and isinstance(watts, (int, float)) and
                math.isfinite(watts) and watts >= 0 and volts and 3 <= volts <= 80):
            payload.update(external_current_a=min(200, watts / volts), current_ts=seen)
        if self.stopped:
            return
        # Each field has its own replay protection on the controller. Omit an
        # unchanged field so it cannot reject a fresh measurement alongside it.
        if payload.get("soc_ts", 0) <= self.sent_timestamps.get("soc_ts", 0):
            for key in ("soc", "voltage", "soc_ts"):
                payload.pop(key, None)
        if payload.get("current_ts", 0) <= self.sent_timestamps.get("current_ts", 0):
            for key in ("external_current_a", "current_ts"):
                payload.pop(key, None)
        if payload:
            await self.transport.publish_external(payload, self.topic)
            self.sent_timestamps.update({key: payload[key] for key in ("soc_ts", "current_ts") if key in payload})
        free = self.data.get("free-power", {})
        if free.get("id") and now >= free.get("end", 0) and self.data["memo"].get("completed_id") != free["id"]:
            self.data["memo"]["completed_id"] = free["id"]
            free["completed"] = 1
            await self.save()
        settings = self.c.snapshot.get("ess", {}).get("settings", {})
        recurring = self.data.get("recharge", {})
        previous_memo = deepcopy(self.data["memo"])
        soc = battery_payload.get("soc")
        if soc is None:
            soc = live_reading(self.c.snapshot.get("state", {}), "soc",
                               monotonic() - self.c.fetched.get("state", 0))
        desired, watts = lease_decision(recurring, free, now, soc,
                                        settings.get("max_charge", 0), self.data["memo"])
        if self.stopped:
            return
        lease_id = hashlib.sha256((self.c.identity + ":" + desired).encode()).hexdigest()[:32] if desired else None
        if self.lease_id and self.lease_id != lease_id:
            await self.api.post("ess/lease", {"id": self.lease_id, "clear": True})
            self.lease_id = None
            self.data["memo"]["lease_id"] = None
        if lease_id and (lease_id != self.lease_id or monotonic() - self.last_lease >= 5):
            await self.api.post("ess/lease", {"id": lease_id, "ttl_s": LEASE_SECONDS, "charge_w": watts})
            self.lease_id, self.last_lease = lease_id, monotonic()
            self.data["memo"]["lease_id"] = lease_id
        if previous_memo != self.data["memo"]:
            await self.save()

    async def peer_request(self, path, fields=None):
        from homeassistant.helpers.aiohttp_client import async_get_clientsession
        host, key = self.options.get("r4875_host"), self.options.get("r4875_token")
        if not host or not key:
            raise ValueError("Configure the charger host and pairing key")
        session = async_get_clientsession(self.hass)
        started = time()
        async with asyncio.timeout(2):
            async with session.request("POST" if fields else "GET", f"http://{host}/api/{path}",
                                       headers={"X-Session-Key": key}, data=fields,
                                       allow_redirects=False) as response:
                raw = await response.content.read(4097)
                if response.status != 200 or len(raw) > 4096:
                    raise ValueError("Charger request failed")
                import json
                value = json.loads(raw)
        if not isinstance(value, dict) or not isinstance(value.get("id"), int):
            raise ValueError("Invalid charger response")
        # Conservative HTTP observation time, matching the former peer freshness rule.
        self.peer_seen[path] = started
        return value

    def view(self, path):
        value = deepcopy(self.data.get(path, {}))
        if not value:
            if path == "recharge":
                value = {"version": 2, "id": 0, "enabled": 0, "day": 7, "start": 0, "end": 1,
                         "victron": 0, "r4875": 0, "victron_w": 0, "r4875_w": 0,
                         "victron_soc": 100, "r4875_soc": 100, "grid_after_target": 0,
                         "timezone": self.hass.config.time_zone}
            else:
                value = {"version": 1, "id": 0, "enabled": 0, "start": 0, "end": 0,
                         "charge_w": 2500, "chargers": 0, "completed": 0}
        now = time()
        peer = deepcopy(self.peer[path])
        keys = RECHARGE_FIELDS if path == "recharge" else FREE_KEYS
        reachable = 0 <= now - self.peer_seen[path] <= 6
        peer.update(reachable=reachable,
                    matching=bool(reachable and peer.get("clock_valid") and
                                  all(peer.get(key) == value.get(key) for key in ("id", *keys) if key not in ("version", "completed"))))
        peer["error"] = self.error
        value.update(peer=peer, utc=int(now), clock_valid=now >= 1767225600, managed_by="Home Assistant")
        if path == "free-power":
            value["phase"] = free_phase(value, now, self.data["memo"].get("completed_id", 0))
            value["active"] = int(value["phase"] == "active")
        else:
            window = recurring_window(value, now)
            battery = self.battery.payload(now) if self.battery else {}
            state = self.c.snapshot.get("state", {})
            soc = battery.get("soc")
            sync = self.c.snapshot.get("battery-input", {})
            sync_elapsed = monotonic() - self.c.fetched.get("battery-input", 0)
            verified = (sync.get("sync_verified") is True and
                        0 <= sync.get("soc_age_s", -1) + sync_elapsed <= 10 and
                        0 <= sync.get("sync_age_s", -1) + sync_elapsed < 15)
            value.update(active=int(bool(window)), battery_valid=int(bool(battery)),
                         battery_soc=soc, battery_voltage=battery.get("voltage"),
                         battery_age_ms=int((now - self.battery.timestamp) * 1000) if battery else None,
                         victron_own_soc=state.get("soc", [None])[0], victron_soc_source="External battery" if battery else "Inverter",
                         victron_target_met=soc is not None and soc >= value["victron_soc"],
                         victron_charging=bool(window and soc is not None and soc < value["victron_soc"]),
                         soc_sync={"verified": verified, "target": sync.get("sync_target"),
                                   "sent": sync.get("sync_sent", 0),
                                   "age_s": sync.get("sync_age_s", -1) + sync_elapsed if sync.get("sync_age_s", -1) >= 0 else -1})
        return value

    async def set_overnight(self, fields, expected_id=None):
        async with self.lock:
            return await self._set_overnight(fields, expected_id)

    async def _set_overnight(self, fields, expected_id=None):
        before = self.view("recharge")
        if expected_id is not None and int(expected_id) != before["id"]:
            raise ApiError("The overnight schedule changed while editing. Reload before saving")
        desired = {**{key: before[key] for key in RECHARGE_KEYS if key in before},
                   **validate_recharge({**before, **fields})}
        if all(before.get(key) == value for key, value in desired.items()):
            await self.save_peer("recharge", desired, before["id"])
            return
        settings = (await self.api.get("ess"))["settings"]
        row = settings["schedules"][0]
        expected = self.data.get("mapped_slot") or schedule_row(before, row)
        if row != expected:
            raise ApiError("Scheduled charge level 1 changed. Reload and review it before changing the overnight plan")
        desired["id"] = max(int(time()), before["id"] + 1)
        mapped = schedule_row(desired, row)
        changes = {"s1_" + key: value for key, value in mapped.items() if row.get(key) != value}
        grid = int(bool(desired.get("grid_after_target", 0)))
        old_grid = settings.get("schedule_grid_after_target", [0])[0]
        if old_grid != self.data.get("mapped_grid", int(bool(before.get("grid_after_target", 0)))):
            raise ApiError("Scheduled charge level 1 grid setting changed. Review it before changing the overnight plan")
        if grid != old_grid:
            changes["s1_grid_after_target"] = grid
        if changes:
            await self.api.post("ess", changes)
            actual = (await self.api.get("ess"))["settings"]
            if actual["schedules"][0] != mapped or actual.get("schedule_grid_after_target", [0])[0] != grid:
                raise ApiError("Native schedule readback did not match; no charger write was sent")
        self.data.update({"recharge": desired, "mapped_slot": mapped, "mapped_grid": grid})
        await self.save()
        await self.save_peer("recharge", desired, before["id"])

    async def set_free(self, cancel=False, fields=None):
        async with self.lock:
            return await self._set_free(cancel, fields)

    async def _set_free(self, cancel=False, fields=None):
        before = self.view("free-power")
        if cancel and not before.get("id"):
            return
        pending = self.data["memo"].get("pending_cancel", {})
        retry_cancel = cancel and not before.get("enabled")
        expected_peer_id = before["id"]
        free_id = None
        if cancel:
            desired = {key: before[key] for key in FREE_KEYS}
            desired.update(enabled=0)
            if retry_cancel and pending.get("id") == before["id"]:
                expected_peer_id = pending["base_id"]
                free_id = pending.get("lease_id")
            elif not retry_cancel:
                free_id = hashlib.sha256((self.c.identity + ":free:" + str(before["id"])).encode()).hexdigest()[:32]
        else:
            desired = {**self.c.draft, **(fields or {})}
            if fields is None and self.c.draft_base != before["id"]:
                raise ApiError("The saved session changed. Reload the draft before arming")
            start, end = int(desired["start"]), int(desired["end"])
            if not 1767225600 <= start < end or end <= time() or start > time() + 366 * 86400 or end - start > 86400:
                raise ValueError("Use a future dated window lasting up to 24 hours")
            charge = int(desired["charge_w"])
            if not 0 <= charge <= 5000 or desired["chargers"] not in (0, 1, False, True):
                raise ValueError("Invalid free-power charge settings")
            desired = {"version": 1, "start": start, "end": end, "charge_w": charge,
                       "chargers": int(bool(desired["chargers"])), "enabled": 1, "completed": 0}
            if all(before.get(key) == value for key, value in desired.items()):
                await self.save_peer("free-power", {**before, **desired}, before["id"])
                return
        if not retry_cancel:
            desired["id"] = max(int(time()), before["id"] + 1)
        if not cancel:
            # Reject a known peer conflict before arming the HA executor. The
            # actual write still performs a fresh revision check below.
            if self.options.get("r4875_host"):
                current = await self.peer_request("free-power")
                check_peer_revision("free-power", desired, current, before["id"])
            elif desired.get("chargers"):
                raise ApiError("Configure the external charger before arming a session that includes it")
        self.data["free-power"] = desired
        if cancel:
            self.data["memo"]["completed_id"] = desired["id"]
            self.data["memo"]["pending_cancel"] = {"id": desired["id"], "base_id": expected_peer_id,
                                                     "lease_id": free_id}
        else:
            self.data["memo"].pop("pending_cancel", None)
        # Cancel intent must survive a lost release reply and an HA restart.
        # An owned lease can then only be cleared or allowed to expire, never
        # renewed from the former armed plan.
        await self.save()
        release_error = None
        if cancel:
            if free_id and self.lease_id == free_id:
                try:
                    await self.api.post("ess/lease", {"id": self.lease_id, "clear": True})
                except (ApiError, OSError, TimeoutError) as err:
                    release_error = err
                else:
                    self.lease_id = None
                    self.data["memo"]["lease_id"] = None
                    await self.save()
        self.c.draft = {key: desired[key] for key in ("start", "end", "charge_w", "chargers")}
        self.c.draft_base = desired["id"]
        self.c.save_options()
        # The charger owns its executor, so its cancellation must be attempted
        # even when the controller's release acknowledgement was lost.
        try:
            await self.save_peer("free-power", desired, expected_peer_id)
        except (ApiError, OSError, TimeoutError, ValueError) as err:
            if not cancel:
                raise ApiError("Session armed in Home Assistant; external charger confirmation is pending") from err
            message = "Cancellation saved in HA; external charger cancellation is unconfirmed"
            if release_error:
                message += "; controller release is unconfirmed and its lease will expire within 20 seconds"
            raise ApiError(message) from err
        if release_error:
            raise ApiError("Cancellation saved; controller release is unconfirmed and its lease will expire within 20 seconds") from release_error
        if cancel:
            self.data["memo"].pop("pending_cancel", None)
            await self.save()

    async def save_peer(self, path, desired, expected_id=None):
        if not self.options.get("r4875_host"):
            if desired.get("r4875") or desired.get("chargers"):
                raise ApiError("Plan saved in HA; configure the external charger before arming it")
            return
        current = await self.peer_request(path)
        keys = RECHARGE_FIELDS if path == "recharge" else ("start", "end", "enabled", "charge_w", "chargers")
        if current.get("id") == desired["id"] and all(current.get(k) == desired[k] for k in keys):
            self.peer[path] = current
            return
        check_peer_revision(path, desired, current, expected_id)
        fields = {key: desired[key] for key in ("id", *keys)}
        fields["now"] = int(time())
        # One POST per explicit edit. Verification retries only GETs.
        await self.peer_request(path, fields)
        for _ in range(10):
            current = await self.peer_request(path)
            self.peer[path] = current
            if current.get("id") == desired["id"] and all(current.get(k) == desired[k] for k in keys):
                return
            await asyncio.sleep(.5)
        raise ApiError("Plan saved in HA; charger confirmation is pending. The write was not repeated")
