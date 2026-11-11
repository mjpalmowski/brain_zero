"""HA migration policy and authenticated battery transport, without equipment."""
import asyncio
from copy import deepcopy
from datetime import datetime
from functools import wraps
import hashlib
import hmac
import importlib.util
from pathlib import Path
import struct
import sys
import types

import pytest

SOURCE = Path(__file__).resolve().parents[1] / "custom_components/brain_zero"
PACKAGE = types.ModuleType("charging_test_package")
PACKAGE.__path__ = [str(SOURCE)]
sys.modules[PACKAGE.__name__] = PACKAGE


def load(name):
    spec = importlib.util.spec_from_file_location(PACKAGE.__name__ + "." + name, SOURCE / (name + ".py"))
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


api, battery, charging = load("api"), load("battery_link"), load("charging")
NIGHT = {"version": 2, "id": 1794228883, "enabled": 1, "day": 7, "start": 84600,
         "end": 19800, "victron": 1, "r4875": 1, "victron_w": 3000, "r4875_w": 4500,
         "victron_soc": 40, "r4875_soc": 100, "grid_after_target": 1, "timezone": "Europe/London"}
EMPTY = {"version": 1, "id": 0, "start": 0, "end": 0, "charge_w": 2500,
         "enabled": 0, "completed": 0, "chargers": 0}


def run_async(function):
    @wraps(function)
    def run(*args, **kwargs):
        return asyncio.run(function(*args, **kwargs))
    return run


def utc(value):
    return datetime.fromisoformat(value).timestamp()


def signed_reply(request, key, soc=78, voltage=52.9, soc_age=1500, voltage_age=2000, sequence=4):
    magic, hi, lo = struct.unpack("<III", request[:12])
    prefix = battery.REPLY.pack(magic, hi, lo, soc, voltage, soc_age, voltage_age, sequence)
    return prefix + hmac.digest(key.encode(), prefix, "sha256")


def test_authenticated_battery_retains_can_age_and_rtt():
    key = "a" * 64
    request = battery.challenge(key, b"12345678")
    assert len(request) == 44
    sample = battery.decode_reply(signed_reply(request, key), request, key, 100, 100.1, .1)
    assert sample.timestamp == pytest.approx(98)
    assert sample.payload(100.1)["soc_ts"] == pytest.approx(98)
    assert sample.payload(108.01) == {}
    assert sample.sequence == 4


def test_battery_replay_wrong_key_stale_can_and_nonfinite_values_rejected():
    key = "b" * 64
    request = battery.challenge(key, b"12345678")
    other = battery.challenge(key, b"87654321")
    fixtures = [signed_reply(other, key), signed_reply(request, "wrong"),
                signed_reply(request, key, soc_age=10000),
                signed_reply(request, key, soc=float("nan")),
                signed_reply(request, key, voltage=float("inf"))]
    for raw in fixtures:
        with pytest.raises(ValueError):
            battery.decode_reply(raw, request, key, 100, 100.1, .1)


@pytest.mark.parametrize("stamp,hours", [("2026-03-29T00:00:00+00:00", 5),
                                        ("2026-10-25T00:00:00+00:00", 7),
                                        ("2026-11-11T00:00:00+00:00", 6)])
def test_overnight_window_crosses_midnight_and_both_london_dst_changes(stamp, hours):
    result = charging.recurring_window(NIGHT, utc(stamp))
    assert result is not None
    assert result["end"] - result["start"] == hours * 3600
    assert charging.recurring_window(NIGHT, result["end"]) is None


def test_dated_completion_survives_clock_rollback_or_restart():
    plan = {**EMPTY, "id": 12, "enabled": 1, "start": 100, "end": 200}
    assert charging.free_phase(plan, 150) == "active"
    assert charging.free_phase(plan, 200) == "finished"
    assert charging.free_phase(plan, 150, completed_id=12) == "finished"


def test_native_schedule_fallback_and_temporary_lower_cap_hysteresis():
    now = utc("2026-11-11T00:30:00+00:00")
    memo = {}
    assert charging.lease_decision(NIGHT, EMPTY, now, 10, 2500, memo) == (None, 0)
    lower = {**NIGHT, "victron_w": 1800}
    ident, watts = charging.lease_decision(lower, EMPTY, now, 39, 2500, memo)
    assert ident.startswith("overnight:") and watts == 1800
    assert charging.lease_decision(lower, EMPTY, now, None, 2500, memo) == (ident, 0)
    assert charging.lease_decision(lower, EMPTY, now, 40, 2500, memo)[1] == 0
    assert charging.lease_decision(lower, EMPTY, now, 39.5, 2500, memo)[1] == 0
    assert charging.lease_decision(lower, EMPTY, now, 38.9, 2500, memo)[1] == 1800
    free = {**EMPTY, "id": 19, "start": now - 10, "end": now + 10, "enabled": 1, "charge_w": 4000}
    assert charging.lease_decision(lower, free, now, 100, 2500, memo) == ("free:19", 2500)


class Store:
    def __init__(self):
        self.saved = []

    async def async_save(self, data):
        self.saved.append(deepcopy(data))


class Controller:
    def __init__(self):
        self.unified = False
        self.enabled = False
        self.posts = []
        self.settings = {"max_charge": 2500, "schedules": [charging.schedule_row(NIGHT, {"discharge": 1})],
                         "schedule_grid_after_target": [1]}

    async def get(self, path):
        if path == "battery-input":
            if not self.unified:
                raise api.ApiError("Unsupported", status=404)
            return {"enabled": self.enabled, "topic": "vebus_listener/external_battery"}
        if path == "ess":
            return {"settings": deepcopy(self.settings)}
        raise AssertionError(path)

    async def post(self, path, fields):
        self.posts.append((path, deepcopy(fields)))
        if path == "battery-input":
            self.enabled = fields["enabled"]
            return {"enabled": self.enabled}
        if path == "ess":
            for key, value in fields.items():
                if key == "s1_grid_after_target":
                    self.settings["schedule_grid_after_target"][0] = value
                else:
                    self.settings["schedules"][0][key.removeprefix("s1_")] = value
        return {"ok": True}


class Publisher:
    def __init__(self):
        self.messages = []

    async def publish_external(self, payload, topic):
        self.messages.append((topic, deepcopy(payload)))


def adapter():
    hass = types.SimpleNamespace(config=types.SimpleNamespace(time_zone="Europe/London"))
    entry = types.SimpleNamespace(options={"charging_adapter": True}, entry_id="entry")
    controller = Controller()
    coordinator = types.SimpleNamespace(hass=hass, entry=entry, api=controller, identity="80456b58f348",
                                        snapshot={"ess": {"settings": {"max_charge": 2500}}}, fetched={},
                                        draft={}, draft_base=None, save_options=lambda: None)
    pub, store = Publisher(), Store()
    result = charging.ChargingAdapter(coordinator, pub, store=store)
    return result, controller, pub, store


@run_async
async def test_legacy_import_is_idempotent_and_never_enables_missing_feature():
    bridge, controller, _, store = adapter()
    await bridge.capture_legacy("recharge", NIGHT)
    await bridge.capture_legacy("free-power", EMPTY)
    await bridge.capture_legacy("recharge", NIGHT)
    assert len(store.saved) == 2
    await bridge.probe()
    assert not bridge.active
    assert controller.posts == []
    assert bridge.data["recharge"]["start"] == 84600
    assert bridge.data["recharge"]["end"] == 19800


@run_async
async def test_capability_enables_once_and_uses_discovered_topic(monkeypatch):
    bridge, controller, pub, _ = adapter()
    controller.unified = True
    bridge.options.update(battery_link_host="battery.local", battery_link_token="a" * 64)
    sample = battery.BatterySample(78, 52.9, 100, 5)
    async def read(*args):
        return sample
    bridge.battery_reader = read
    monkeypatch.setattr(charging, "time", lambda: 101)
    await bridge.tick()
    bridge.probe_at = 0
    await bridge.tick()
    assert controller.posts == [("battery-input", {"enabled": True})]
    assert pub.messages[-1] == ("vebus_listener/external_battery", {"soc": 78, "voltage": 52.9, "soc_ts": 100})


@run_async
async def test_free_lease_renews_short_ttl_then_clears_and_does_not_replay(monkeypatch):
    bridge, controller, _, _ = adapter()
    controller.unified = True
    now = utc("2026-11-11T12:00:00+00:00")
    clock = {"utc": now, "ms": 100}
    monkeypatch.setattr(charging, "time", lambda: clock["utc"])
    monkeypatch.setattr(charging, "monotonic", lambda: clock["ms"])
    bridge.data["free-power"] = {**EMPTY, "id": 32, "start": now - 10, "end": now + 15,
                                 "enabled": 1, "charge_w": 4000}
    baseline = deepcopy(controller.settings)
    await bridge.tick()
    first = [fields for path, fields in controller.posts if path == "ess/lease"][-1]
    assert first["ttl_s"] <= 30 and first["charge_w"] == 2500
    assert len(first["id"]) == 32
    clock["utc"] += 6; clock["ms"] += 6
    await bridge.tick()
    leases = [fields for path, fields in controller.posts if path == "ess/lease"]
    assert leases[-1]["id"] == first["id"]
    clock["utc"] += 10; clock["ms"] += 10
    await bridge.tick()
    assert controller.posts[-1] == ("ess/lease", {"id": first["id"], "clear": True})
    count = len(controller.posts)
    clock["utc"] = now; clock["ms"] += 2
    await bridge.tick()
    assert len(controller.posts) == count
    assert controller.settings == baseline


@run_async
async def test_explicit_overnight_edit_uses_native_slot_cas_without_changing_global_cap():
    bridge, controller, _, _ = adapter()
    bridge.supported = True
    bridge.data["recharge"] = deepcopy(NIGHT)
    calls = []
    async def peer(path, desired, expected_id=None):
        calls.append((path, deepcopy(desired)))
    bridge.save_peer = peer
    await bridge.set_overnight({"victron_soc": 45}, NIGHT["id"])
    assert controller.posts == [("ess", {"s1_soc": 45})]
    assert controller.settings["max_charge"] == 2500
    assert len(calls) == 1
    controller.settings["schedules"][0]["soc"] = 46
    with pytest.raises(api.ApiError, match="changed"):
        await bridge.set_overnight({"victron_soc": 50})
    assert len(controller.posts) == 1


@run_async
async def test_charger_explicit_noop_verification_does_not_repeat_post():
    bridge, _, _, _ = adapter()
    bridge.options.update(r4875_host="charger.local", r4875_token="b" * 64)
    current = deepcopy(NIGHT)
    calls = []
    async def peer(path, fields=None):
        calls.append(fields)
        return deepcopy(current)
    bridge.peer_request = peer
    await bridge.save_peer("recharge", NIGHT)
    assert calls == [None]
    current["id"] += 1
    with pytest.raises(api.ApiError, match="changed"):
        await bridge.save_peer("recharge", {**NIGHT, "id": NIGHT["id"] + 2}, NIGHT["id"])
    assert calls == [None, None]


@run_async
async def test_restart_renews_same_owned_lease_and_cancel_persists_without_replay(monkeypatch):
    bridge, controller, _, store = adapter()
    controller.unified = True
    now = utc("2026-11-11T12:00:00+00:00")
    monkeypatch.setattr(charging, "time", lambda: now)
    bridge.data["free-power"] = {**EMPTY, "id": 77, "start": now - 10, "end": now + 100,
                                 "enabled": 1, "charge_w": 1800}
    await bridge.tick()
    original_id = bridge.lease_id
    restarted, next_controller, _, next_store = adapter()
    next_controller.unified = True
    restarted.restore(store.saved[-1])
    assert restarted.lease_id == original_id
    await restarted.tick()
    assert next_controller.posts[-1] == ("ess/lease", {"id": original_id, "ttl_s": 20, "charge_w": 1800})
    await restarted.set_free(cancel=True)
    assert next_controller.posts[-1] == ("ess/lease", {"id": original_id, "clear": True})
    final, _, _, _ = adapter()
    final.restore(next_store.saved[-1])
    assert final.lease_id is None
    assert final.view("free-power")["phase"] == "cancelled"
    assert final.data["free-power"]["enabled"] == 0


@run_async
async def test_lost_cancel_reply_cannot_renew_or_rearm_after_restart_and_still_cancels_charger(monkeypatch):
    bridge, controller, _, store = adapter()
    controller.unified = True
    now = utc("2026-11-11T12:00:00+00:00")
    monkeypatch.setattr(charging, "time", lambda: now)
    bridge.data["free-power"] = {**EMPTY, "id": 88, "start": now - 10, "end": now + 3600,
                                 "enabled": 1, "charge_w": 1800, "chargers": 1}
    await bridge.tick()
    owned = bridge.lease_id
    original_post = controller.post
    async def lost_reply(path, fields):
        result = await original_post(path, fields)
        if path == "ess/lease" and fields.get("clear"):
            raise api.ApiError("Clear reply was lost")
        return result
    controller.post = lost_reply
    peer_calls = []
    async def cancel_peer(path, desired, expected_id=None):
        # The durable disabled record already exists before either release.
        assert store.saved[-1]["free-power"]["enabled"] == 0
        peer_calls.append((path, deepcopy(desired), expected_id))
    bridge.save_peer = cancel_peer
    with pytest.raises(api.ApiError, match="Cancellation saved"):
        await bridge.set_free(cancel=True)
    assert peer_calls[0][1]["enabled"] == 0 and peer_calls[0][2] == 88
    assert store.saved[-1]["free-power"]["enabled"] == 0
    # Even while the clear keeps failing, the original lease is never renewed.
    start = len(controller.posts)
    with pytest.raises(api.ApiError):
        await bridge.tick()
    assert controller.posts[start:] == [("ess/lease", {"id": owned, "clear": True})]
    restarted, next_controller, _, _ = adapter()
    next_controller.unified = True
    restarted.restore(store.saved[-1])
    assert restarted.view("free-power")["phase"] == "cancelled"
    await restarted.tick()
    assert [fields for path, fields in next_controller.posts if path == "ess/lease"] == [{"id": owned, "clear": True}]


@run_async
async def test_independent_sample_dedup_does_not_send_replay_with_fresh_current(monkeypatch):
    bridge, controller, pub, _ = adapter()
    controller.unified = True
    clock = {"utc": 100.0, "mono": 100.0}
    monkeypatch.setattr(charging, "time", lambda: clock["utc"])
    monkeypatch.setattr(charging, "monotonic", lambda: clock["mono"])
    bridge.battery = battery.BatterySample(20, 50, 99, 1)
    bridge.peer["recharge"] = {"dc_w": 1000, "clock_valid": 1}
    bridge.peer_seen["recharge"] = 99.5
    await bridge.tick()
    assert set(pub.messages[-1][1]) == {"soc", "voltage", "soc_ts", "external_current_a", "current_ts"}
    clock.update(utc=102, mono=102)
    bridge.peer_seen["recharge"] = 101.5
    await bridge.tick()
    assert pub.messages[-1][1] == {"external_current_a": 20, "current_ts": 101.5}
    await bridge.tick()
    assert len(pub.messages) == 2


@run_async
async def test_fresh_external_current_never_revives_expired_battery_soc_for_a_lease(monkeypatch):
    bridge, controller, pub, _ = adapter()
    controller.unified = True
    now = utc("2026-11-11T00:30:00+00:00")
    monkeypatch.setattr(charging, "time", lambda: now)
    bridge.data["recharge"] = {**NIGHT, "victron_w": 1800}
    bridge.battery = battery.BatterySample(20, 50, now - 11, 1)
    bridge.c.snapshot["state"] = {"bat_v": [50, 0]}
    bridge.c.fetched["state"] = charging.monotonic()
    bridge.peer["recharge"] = {"dc_w": 1000, "clock_valid": 1}
    bridge.peer_seen["recharge"] = now - .1
    await bridge.tick()
    assert pub.messages[-1][1]["external_current_a"] == 20
    assert "soc" not in pub.messages[-1][1]
    assert [fields["charge_w"] for path, fields in controller.posts if path == "ess/lease"] == [0]


@run_async
async def test_lower_overnight_cap_uses_fresh_inverter_soc_then_holds_zero_when_both_stale(monkeypatch):
    bridge, controller, _, _ = adapter()
    controller.unified = True
    now = utc("2026-11-11T00:30:00+00:00")
    monkeypatch.setattr(charging, "time", lambda: now)
    bridge.data["recharge"] = {**NIGHT, "victron_w": 1800}
    bridge.battery = battery.BatterySample(90, 50, now - 11, 1)
    bridge.c.snapshot["state"] = {"soc": [20, 0], "multi_age_s": 0}
    bridge.c.fetched["state"] = charging.monotonic()
    await bridge.tick()
    first = controller.posts[-1][1]
    assert first["charge_w"] == 1800
    bridge.c.fetched["state"] -= 11
    bridge.last_lease = 0
    await bridge.tick()
    assert controller.posts[-1] == ("ess/lease", {"id": first["id"], "ttl_s": 20, "charge_w": 0})
    assert controller.settings["max_charge"] == 2500


@run_async
async def test_explicit_cancel_retry_survives_restart_preserves_cas_and_never_rearms(monkeypatch):
    bridge, controller, _, store = adapter()
    controller.unified = True
    now = utc("2026-11-11T12:00:00+00:00")
    monkeypatch.setattr(charging, "time", lambda: now)
    original = {**EMPTY, "id": 90, "start": now - 10, "end": now + 3600,
                "enabled": 1, "charge_w": 1800, "chargers": 1}
    bridge.data["free-power"] = deepcopy(original)
    async def unavailable(*args):
        raise api.ApiError("Charger unavailable")
    bridge.save_peer = unavailable
    with pytest.raises(api.ApiError, match="Cancellation saved in HA"):
        await bridge.set_free(cancel=True)
    saved = deepcopy(store.saved[-1])
    revision = saved["free-power"]["id"]
    assert saved["memo"]["pending_cancel"]["base_id"] == 90
    restarted, _, _, _ = adapter()
    restarted.restore(saved)
    restarted.options["r4875_host"] = "charger.local"
    current, calls = deepcopy(original), []
    async def peer(path, fields=None):
        calls.append(deepcopy(fields))
        if fields:
            current.update({key: value for key, value in fields.items() if key != "now"})
        return deepcopy(current)
    restarted.peer_request = peer
    await restarted.set_free(cancel=True)
    assert current["id"] == revision and current["enabled"] == 0
    assert len([call for call in calls if call]) == 1
    assert restarted.data["free-power"]["id"] == revision
    assert "pending_cancel" not in restarted.data["memo"]
    await restarted.set_free(cancel=True)
    assert len([call for call in calls if call]) == 1
    # A different peer revision remains protected even after HA restarts.
    restarted.restore(saved)
    current["id"] = revision + 1
    count = len([call for call in calls if call])
    with pytest.raises(api.ApiError, match="Cancellation saved in HA"):
        await restarted.set_free(cancel=True)
    assert len([call for call in calls if call]) == count
    assert restarted.data["free-power"]["enabled"] == 0


@run_async
async def test_first_arm_adopts_only_fresh_terminal_older_charger_session(monkeypatch):
    bridge, _, _, _ = adapter()
    now = utc("2026-11-11T12:00:00+00:00")
    monkeypatch.setattr(charging, "time", lambda: now)
    bridge.data["free-power"] = {**EMPTY, "chargers": 1}
    bridge.options["r4875_host"] = "charger.local"
    current = {**EMPTY, "id": 1793480849, "start": now - 7200, "end": now - 3600,
               "enabled": 1, "completed": 1, "phase": "finished", "active": 0, "clock_valid": 1}
    calls = []
    async def peer(path, fields=None):
        calls.append(deepcopy(fields))
        if fields:
            current.update({key: value for key, value in fields.items() if key != "now"})
        return deepcopy(current)
    bridge.peer_request = peer
    fields = {"start": now + 60, "end": now + 3660, "charge_w": 1800, "chargers": 1}
    await bridge.set_free(fields=fields)
    assert bridge.data["free-power"]["enabled"] == 1
    assert len([call for call in calls if call]) == 1
    rejected, _, _, store = adapter()
    rejected.data["free-power"] = deepcopy(EMPTY)
    rejected.options["r4875_host"] = "charger.local"
    rejected.peer_request = peer
    current.update(id=1793480849, start=now - 60, end=now + 3600, enabled=1, completed=0, active=1)
    count = len(calls)
    with pytest.raises(api.ApiError, match="changed"):
        await rejected.set_free(fields=fields)
    assert len(calls) == count + 1 and calls[-1] is None
    assert rejected.data["free-power"]["enabled"] == 0 and store.saved == []


def test_verified_entities_require_fresh_authenticated_peer_and_soc_status(monkeypatch):
    bridge, _, _, _ = adapter()
    now = utc("2026-11-11T00:30:00+00:00")
    clock = {"utc": now, "mono": 100}
    monkeypatch.setattr(charging, "time", lambda: clock["utc"])
    monkeypatch.setattr(charging, "monotonic", lambda: clock["mono"])
    bridge.data["recharge"] = deepcopy(NIGHT)
    bridge.peer["recharge"] = {**NIGHT, "clock_valid": 1}
    bridge.peer_seen["recharge"] = now
    bridge.c.snapshot["battery-input"] = {"sync_verified": True, "soc_age_s": 1,
                                           "sync_age_s": 2, "sync_target": 78, "sync_sent": 4}
    bridge.c.fetched["battery-input"] = 100
    assert bridge.view("recharge")["peer"]["matching"]
    assert bridge.view("recharge")["soc_sync"]["verified"]
    bridge.peer["recharge"]["clock_valid"] = 0
    assert not bridge.view("recharge")["peer"]["matching"]
    clock.update(utc=now + 10, mono=110)
    assert not bridge.view("recharge")["peer"]["matching"]
    assert not bridge.view("recharge")["soc_sync"]["verified"]


@run_async
async def test_fast_adapter_tick_uses_listener_updates_without_rescheduling_poll():
    bridge, _, _, _ = adapter()
    bridge.supported = True
    bridge.c.missing_site_sections = set()
    updates = []
    bridge.c.publish_auxiliary = lambda: updates.append(deepcopy(bridge.c.snapshot))
    def reset_forbidden():
        raise AssertionError("2 s adapter tick must not reset the 5 s controller timer")
    bridge.c.publish = reset_forbidden
    async def no_request():
        pass
    bridge.tick = no_request
    for _ in range(10):
        await bridge._tick_event(None)
    assert len(updates) == 10
    assert all("recharge" in update and "free-power" in update for update in updates)


@run_async
async def test_invalid_import_cannot_arm_a_malformed_free_power_plan():
    bridge, _, _, store = adapter()
    with pytest.raises(ValueError):
        await bridge.capture_legacy("free-power", {**EMPTY, "enabled": 1})
    assert "free-power" not in bridge.data and store.saved == []
