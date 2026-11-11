"""Protocol and traffic guarantees without any connection to real equipment."""
import asyncio
import copy
import importlib.util
import json
from pathlib import Path
import sys
import types

import pytest

ROOT = Path(__file__).resolve().parents[2]
PACKAGE = ROOT / "home-assistant" / "custom_components" / "brain_zero"
pkg = types.ModuleType("brain_zero_test_package")
pkg.__path__ = [str(PACKAGE)]
sys.modules[pkg.__name__] = pkg


def load(name):
    spec = importlib.util.spec_from_file_location(pkg.__name__ + "." + name, PACKAGE / (name + ".py"))
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


models, api = load("models"), load("api")
# HA adapters are exercised by live setup; this stub isolates the command protocol.
exceptions = types.ModuleType("homeassistant.exceptions")
exceptions.HomeAssistantError = type("HomeAssistantError", (Exception,), {})
updates = types.ModuleType("homeassistant.helpers.update_coordinator")


class Coordinator:
    def __init__(self, *args, **kwargs):
        self.hass = args[0]
        self.last_update_success = True
    def async_set_updated_data(self, data):
        self.data = data
    def async_update_listeners(self):
        self.listener_updates = getattr(self, "listener_updates", 0) + 1


updates.DataUpdateCoordinator = Coordinator
updates.UpdateFailed = type("UpdateFailed", (Exception,), {})
for name in ("homeassistant", "homeassistant.helpers"):
    sys.modules.setdefault(name, types.ModuleType(name))
sys.modules[exceptions.__name__] = exceptions
sys.modules[updates.__name__] = updates
coord = load("coordinator")


class Clock:
    def __init__(self):
        self.now = 100.0
    def __call__(self):
        return self.now
    async def sleep(self, delta):
        self.now += delta


class Exchange:
    def __init__(self, clock):
        self.clock, self.calls = clock, []
        self.response = b'{"status":200,"data":{"ok":true}}'
        self.fail = False

    async def __call__(self, topic, reply_topic, payload):
        message = json.loads(payload)
        self.calls.append((self.clock(), topic, reply_topic, message))
        if self.fail:
            raise OSError("broker disconnected")
        return self.response


IDENTITY = "001122aabbcc"


def test_single_shared_broker_budget_and_numeric_wire_format():
    async def run():
        clock = Clock()
        exchange = Exchange(clock)
        client = api.BrainApi(exchange, IDENTITY, clock=clock, sleep=clock.sleep)
        await asyncio.gather(*(client.get("state") for _ in range(8)))
        assert all(b[0] - a[0] >= .5 for a, b in zip(exchange.calls, exchange.calls[1:]))
        assert (await client.post("ess", {"grid_setpoint": -15.0, "min_soc": 12.5, "run": True}))["ok"]
        _, topic, reply_topic, request = exchange.calls[-1]
        assert topic == f"brain_zero/{IDENTITY}/request"
        assert reply_topic == f"brain_zero/{IDENTITY}/reply/{request['id']}"
        assert request["op"] == "set" and request["expires"] > 0
        assert request["fields"] == {"grid_setpoint": "-15", "min_soc": "12.5", "run": "1"}
        assert len({c[3]["id"] for c in exchange.calls}) == len(exchange.calls)
    asyncio.run(run())


def test_backoff_blocks_reads_and_does_not_retry_writes():
    async def run():
        clock = Clock()
        exchange = Exchange(clock)
        exchange.fail = True
        client = api.BrainApi(exchange, IDENTITY, clock=clock, sleep=clock.sleep)
        with pytest.raises(api.ApiError, match="not retried"):
            await client.post("inverter", {"inverter_mode": "On"})
        for _ in range(12):
            with pytest.raises(api.ApiError, match="backing off"):
                await client.get("state")
        assert len(exchange.calls) == 1
        clock.now += 5
        with pytest.raises(api.ApiError, match="No MQTT response"):
            await client.get("state")
        assert client.retry_at == clock.now + 10 and len(exchange.calls) == 2
    asyncio.run(run())


@pytest.mark.parametrize("raw", [b'x', b'[]', b'{"status":200,"data":[]}', b'{"p":"' + b'x' * 17000 + b'"}'])
def test_malformed_and_oversized_responses_rejected(raw):
    async def run():
        clock = Clock()
        exchange = Exchange(clock)
        exchange.response = raw
        client = api.BrainApi(exchange, IDENTITY, clock=clock, sleep=clock.sleep)
        with pytest.raises(api.ApiError):
            await client.get("state")
    asyncio.run(run())


def test_unknown_operations_and_bad_ids_never_reach_broker():
    async def run():
        clock = Clock()
        exchange = Exchange(clock)
        client = api.BrainApi(exchange, IDENTITY, clock=clock, sleep=clock.sleep)
        for endpoint in ("raw_bus", "../upload", "settings/read", "state"):
            with pytest.raises(api.ApiError):
                await client.post(endpoint, {})
        assert not exchange.calls
    asyncio.run(run())
    for identity in ("brain_zero/+", "brain-zero.local", ""):
        with pytest.raises(ValueError):
            api.BrainApi(None, identity)


def fixture(name):
    return json.loads((Path(__file__).parent / "fixtures" / "brain_zero" / (name + ".json")).read_text())


class FakeApi:
    def __init__(self):
        self.data = {k: fixture(k) for k in ("state", "ess", "inverter", "charger", "recharge", "free-power")}
        self.posts, self.gets = [], []
        self.reject_readback = False
        self.host, self.url = "s3.local", "http://s3.local"
        self.requests = self.errors = 0
    async def get(self, endpoint):
        self.gets.append(endpoint)
        return copy.deepcopy(self.data[endpoint])
    async def post(self, endpoint, fields):
        self.posts.append((endpoint, fields.copy()))
        if endpoint == "ess" and not self.reject_readback:
            self.data[endpoint]["settings"].update(fields)
        return {"ok": True}


def new_coordinator():
    entry = types.SimpleNamespace(unique_id="001122aabbcc", options={})
    def update(e, **kwargs):
        for k, v in kwargs.items():
            setattr(e, k, copy.deepcopy(v))
    hass = types.SimpleNamespace(config_entries=types.SimpleNamespace(async_update_entry=update))
    return coord.BrainCoordinator(hass, entry, FakeApi())


def test_auxiliary_updates_never_reset_poll_timer_or_hide_controller_failure():
    c = new_coordinator()
    c.last_update_success = False
    c.snapshot = {"state": {"heap": 100}, "recharge": {"id": 1}}
    def reset_forbidden(_data):
        raise AssertionError("Auxiliary update reset the controller polling timer")
    c.async_set_updated_data = reset_forbidden
    for tick in range(10):
        c.snapshot["recharge"] = {"id": tick}
        c.publish_auxiliary()
    assert c.data["recharge"]["id"] == 9
    assert c.listener_updates == 10
    assert c.last_update_success is False


def test_five_second_poll_progresses_during_two_second_adapter_notifications():
    async def simulate(publish_name):
        c = new_coordinator()
        clock = {"now": 0, "due": 5}
        def manual_update(data):
            # HA 2026.11 async_set_updated_data resets the refresh interval.
            c.data = data
            clock["due"] = clock["now"] + 5
        c.async_set_updated_data = manual_update
        for second in range(1, 31):
            clock["now"] = second
            if second % 2 == 0:
                getattr(c, publish_name)()
            if second >= clock["due"]:
                await c._async_update_data()
                clock["due"] = second + 5
        return c.api.gets.count("state")
    assert asyncio.run(simulate("publish_auxiliary")) == 6
    # Reproduces the prior starvation, so the regression exercises its cause.
    assert asyncio.run(simulate("publish")) == 0


def test_standard_build_keeps_ess_online_without_site_endpoints():
    async def run():
        calls = []

        async def exchange(topic, reply_topic, payload):
            key = json.loads(payload)["path"]
            calls.append(key)
            if key in api.SITE_PATHS:
                return b'{"status":404,"data":{"error":"unsupported"}}'
            return json.dumps({"status": 200, "data": fixture(key)}).encode()

        c = new_coordinator()
        clock = Clock()
        c.api = api.BrainApi(exchange, IDENTITY, clock=clock, sleep=clock.sleep)
        data = await c._async_update_data()
        assert set(data) == {"state", "ess", "inverter"}
        assert c.cached_snapshot()["online"]
        assert c.api.failures == 0
        await c._async_update_data()
        assert calls.count("recharge") == calls.count("free-power") == 1
        with pytest.raises(api.SiteFeatureUnavailable):
            await c.set_overnight({"enabled": True})
        assert c.cached_snapshot()["online"]
    asyncio.run(run())


def ac_coordinator():
    c = new_coordinator()
    c.api.data["state"]["ac_input"] = {"revision": 42, "requested": False, "ignored": False,
        "grid_connected": True, "phase": "connected", "can_enable": True,
        "settings": {"reconnect_soc": 10, "reconnect_v": 50, "minutes": 30}}
    old = c.api.post
    async def post(endpoint, fields):
        await old(endpoint, fields)
        a = c.api.data["state"]["ac_input"]
        if endpoint == "ac-input/settings":
            a["settings"] = {k: v for k, v in fields.items() if k != "base"}
            a["revision"] += 1
        if endpoint == "ac-input":
            a.update(requested=bool(fields["ignore"]), ignored=bool(fields["ignore"]),
                     grid_connected=not fields["ignore"], phase="islanded" if fields["ignore"] else "connected")
            a["revision"] += 1
    c.api.post = post
    return c


def test_ac_input_separate_config_and_actuation_with_readback():
    async def run():
        c = ac_coordinator()
        await c.set_ac_input({"reconnect_soc": 12}, base=42)
        assert c.api.posts == [("ac-input/settings", {"reconnect_soc": 12, "reconnect_v": 50, "minutes": 30, "base": 42})]
        await c.set_ac_input({"ignore": True}, base=43)
        assert c.api.posts[-1] == ("ac-input", {"ignore": 1, "base": 43})
        await c.set_ac_input({"ignore": False}, base=44)
        assert c.api.data["state"]["ac_input"]["grid_connected"] is True
        for key in ("ignore_ac_input", "ac_input_ignored", "grid_accepted", "reconnect_soc"):
            assert c.value(models.BY_KEY[key]) is not None
    asyncio.run(run())


def pv_coordinator():
    c = new_coordinator()
    c.api.data['state']['pv_disable'] = {'revision': 7, 'requested': False, 'disabled': False,
        'can_enable': True, 'settings': {'seconds': 60}, 'transport': {'verified': True}}
    old = c.api.post
    async def post(endpoint, fields):
        await old(endpoint, fields)
        a = c.api.data['state']['pv_disable']
        if endpoint == 'pv-disable/settings':
            a['settings']['seconds'] = fields['seconds']
        elif endpoint == 'pv-disable':
            a.update(requested=bool(fields['disable']), disabled=bool(fields['disable']))
        a['revision'] += 1
    c.api.post = post
    return c


def test_pv_control_separate_duration_readback_and_no_enable_replay():
    async def run():
        c = pv_coordinator()
        await c.set_pv_disable({'seconds': 10, 'disable': True}, base=7)
        assert c.api.posts == [('pv-disable/settings', {'seconds': 10, 'base': 7}),
                               ('pv-disable', {'disable': 1, 'base': 8})]
        await c.set_pv_disable({'disable': True}, base=9)
        assert len(c.api.posts) == 2  # repeated enable does not renew the lease
        await c.set_pv_disable({'disable': False}, base=9)
        assert c.api.posts[-1] == ('pv-disable', {'disable': 0, 'base': 9})
        assert c.value(models.BY_KEY['pv_disabled']) is False
        assert c.value(models.BY_KEY['pv_restore_seconds']) == 10
    asyncio.run(run())


def test_pv_control_rejects_stale_revision_invalid_duration_and_grid_connection():
    async def run():
        c = pv_coordinator()
        for fields, base in (({'disable': True}, 6), ({'seconds': 9}, 7), ({'seconds': 301}, 7), ({'raw': 8}, 7)):
            with pytest.raises((api.ApiError, ValueError)):
                await c.set_pv_disable(fields, base)
        c.api.data['state']['pv_disable'].update(can_enable=False, blocked='Confirmed island required')
        with pytest.raises(api.ApiError, match='island'):
            await c.set_pv_disable({'disable': True}, 7)
        assert not c.api.posts
    asyncio.run(run())


def test_pv_control_does_not_accept_ack_without_actual_readback_or_retry():
    async def run():
        c = pv_coordinator()
        async def no_readback(endpoint, fields):
            c.api.posts.append((endpoint, fields))
            c.api.data['state']['pv_disable'].update(requested=True, disabled=None, transport={'verified': False, 'acknowledged': True})
        c.api.post = no_readback
        old_sleep = coord.asyncio.sleep
        async def fast(_): pass
        coord.asyncio.sleep = fast
        try:
            with pytest.raises(api.ApiError, match='readback pending'):
                await c.set_pv_disable({'disable': True}, 7)
        finally:
            coord.asyncio.sleep = old_sleep
        assert len(c.api.posts) == 1
    asyncio.run(run())


def test_ac_input_stale_revision_blocked_and_invalid_requests_do_not_write():
    async def run():
        c = ac_coordinator()
        for fields, base in (({"ignore": True}, 41), ({"minutes": 0}, 42), ({"reconnect_v": 49.95}, 42), ({"raw": 8}, 42)):
            with pytest.raises((api.ApiError, ValueError)):
                await c.set_ac_input(fields, base)
        c.api.data["state"]["ac_input"]["can_enable"] = False
        with pytest.raises(api.ApiError):
            await c.set_ac_input({"ignore": True}, 42)
        assert not c.api.posts
    asyncio.run(run())


def test_ac_input_protection_cancellation_is_not_reported_as_success_or_retried():
    async def run():
        c = ac_coordinator()
        async def cancelled(endpoint, fields):
            c.api.posts.append((endpoint, fields))
            c.api.data["state"]["ac_input"].update(requested=False, reason="Charging takes priority")
        c.api.post = cancelled
        with pytest.raises(api.ApiError, match="Charging takes priority"):
            await c.set_ac_input({"ignore": True}, 42)
        assert len(c.api.posts) == 1
    asyncio.run(run())


def test_idle_poll_is_bounded_and_does_not_start_charger_reads():
    async def run():
        c = new_coordinator()
        await c._async_update_data()
        c.api.gets.clear()
        await c._async_update_data()
        assert c.api.gets == ["state"]
        for k in coord.PERIODS:
            c.fetched[k] = 0
        c.api.gets.clear()
        await c._async_update_data()
        assert len(c.api.gets) == 2 and c.api.gets[0] == "state"
        assert "charger" not in c.api.gets and not c.api.posts
    asyncio.run(run())


def test_noop_ess_and_changed_field_only_preserve_other_settings():
    async def run():
        c = new_coordinator(); old = copy.deepcopy(c.api.data["ess"]["settings"])
        await c.set_ess({"grid_setpoint": old["grid_setpoint"]})
        assert not c.api.posts
        await c.set_ess({"grid_setpoint": old["grid_setpoint"] + 1})
        assert c.api.posts == [("ess", {"grid_setpoint": old["grid_setpoint"] + 1})]
        assert {k:v for k,v in c.api.data["ess"]["settings"].items() if k != "grid_setpoint"} == {k:v for k,v in old.items() if k != "grid_setpoint"}
        c.api.reject_readback = True
        with pytest.raises(api.ApiError, match="readback"):
            await c.set_ess({"grid_setpoint": 321})
        assert len(c.api.posts) == 2
    asyncio.run(run())


def test_disabled_limit_presets_do_not_change_s3_until_enabled():
    async def run():
        c = new_coordinator(); c.api.data["ess"]["settings"]["max_import"] = -1
        await c.set_limit(models.BY_KEY["max_import"], 1234)
        assert not c.api.posts and c.entry.options["limit_presets"]["max_import"] == 1234
        await c.set_limit(models.BY_KEY["max_import_enabled"], True)
        assert c.api.posts == [("ess", {"max_import": 1234})]
        await c.set_limit(models.BY_KEY["max_import_enabled"], False)
        assert c.api.data["ess"]["settings"]["max_import"] == -1
    asyncio.run(run())


def test_free_power_date_edits_are_drafts_and_conflicts_do_not_post():
    async def run():
        c = new_coordinator(); await c.fetch("free-power")
        await c.apply(models.BY_KEY["free_power_start"], 1793545200)
        assert not c.api.posts and c.entry.options["free_power_draft"]["start"] == 1793545200
        c.api.data["free-power"]["id"] += 1
        with pytest.raises(api.ApiError, match="changed"):
            await c.free_power()
        assert not c.api.posts
    asyncio.run(run())


def test_busy_commands_rejected_and_unknown_bus_controls_never_post():
    async def run():
        c = new_coordinator()
        await c.command_lock.acquire()
        with pytest.raises(exceptions.HomeAssistantError, match="still being verified"):
            await c.apply(models.BY_KEY["run"], True)
        c.command_lock.release()
        with pytest.raises(ValueError, match="Unsupported"):
            await c.set_inverter("unknown_ram_write", 10)
        assert not c.api.posts
    asyncio.run(run())


def test_command_completion_refreshes_telemetry_before_rescheduling_poll():
    async def run():
        c = new_coordinator(); await c.fetch("free-power")
        c.fetched["state"] = 0
        await c.apply(models.BY_KEY["free_power_start"], 1793545200)
        assert c.api.gets[-1] == "state" and not c.api.posts
        assert c.value(models.BY_KEY["ac_in_voltage"]) is not None
    asyncio.run(run())


def test_dashboard_snapshot_is_cached_redacted_and_independent():
    async def run():
        c = new_coordinator(); await c._async_update_data()
        c.api.gets.clear()
        c.snapshot["state"]["mqtt"] = {"private": "not for the dashboard"}
        for _ in range(25):
            result = c.cached_snapshot()
            assert result["online"] and "mqtt" not in result["snapshot"]["state"]
            result["snapshot"]["ess"]["settings"]["grid_setpoint"] = 1234
        assert not c.api.gets and not c.api.posts
        assert c.snapshot["ess"]["settings"]["grid_setpoint"] != 1234
    asyncio.run(run())


def test_batch_schedules_validate_conflicts_one_write_and_exact_readback():
    async def run():
        c = new_coordinator(); original = copy.deepcopy(c.api.data["ess"]["settings"]["schedules"])
        desired = copy.deepcopy(original); desired[2]["soc"] = 77; desired[4]["start"] = 1234
        with pytest.raises(api.ApiError, match="changed"):
            await c.set_schedules(desired, desired)
        assert not c.api.posts
        await c.set_schedules(original, original)
        assert not c.api.posts
        old_post = c.api.post
        async def post(endpoint, fields):
            c.api.posts.append((endpoint, fields.copy()))
            for key, value in fields.items():
                slot, field = key.split("_", 1)
                c.api.data["ess"]["settings"]["schedules"][int(slot[1:])-1][field] = value
        c.api.post = post
        await c.set_schedules(desired, original)
        assert c.api.posts == [("ess", {"s3_soc": 77, "s5_start": 1234})]
        c.api.post = old_post
        with pytest.raises(ValueError):
            await c.set_schedules([None] * 5, original)
        with pytest.raises(api.ApiError, match="changed"):
            await c.set_overnight({"enabled": True}, expected_id=0)
        async def charger_read():
            return c.api.data["charger"]
        c.refresh_charger = charger_read
        with pytest.raises(api.ApiError, match="changed while editing"):
            await c.set_charger({"charge_current": 50}, base="stale")
    asyncio.run(run())


def test_validation_and_freshness_guards():
    for bad in (True, None, "nan", "inf", -1, 101, 12.5):
        with pytest.raises(ValueError):
            models.number(bad, 0, 100)
    for bad in ("24:00", "12:60", "01:02:99"):
        with pytest.raises(ValueError):
            models.seconds(bad)
    state = fixture("state")
    assert models.live_reading(state, "bat_v", 0) == state["bat_v"][0]
    assert models.live_reading(state, "bat_v", 6) is None
    state["bat_v"] = [float("nan"), 0]
    assert models.live_reading(state, "bat_v") is None
    assert len(models.SPECS) == len(models.BY_KEY)


def test_charger_write_uses_fresh_base_and_requires_all_readbacks():
    async def run():
        c = new_coordinator()
        before = {"state": "done", "parameters": [
            {"key": "charge_current", "value": 50, "raw": 50, "supported": True, "minimum": 0, "maximum": 70, "age_s": 0},
            {"key": "absorption_voltage", "value": 55.2, "raw": 5520, "supported": True, "minimum": 48, "maximum": 64, "age_s": 0},
            {"key": "float_voltage", "value": 54, "raw": 5400, "supported": True, "minimum": 48, "maximum": 64, "age_s": 0}]}
        async def read():
            return copy.deepcopy(before)
        async def verified():
            data = copy.deepcopy(before); data["saving"] = True
            for p in data["parameters"]:
                p["verified"] = True
                if p["key"] == "charge_current":
                    p["raw"] = 51; p["value"] = 51
            return data
        c.refresh_charger, c.wait_charger = read, verified
        await c.set_charger({"charge_current": 51})
        assert c.api.posts == [("charger", {"charge_current": 51, "absorption_voltage": 55.2, "float_voltage": 54, "base": "50,5520,5400"})]
        c.api.posts.clear()
        await c.set_charger({"charge_current": 50})
        assert not c.api.posts
        with pytest.raises(ValueError, match="Float"):
            await c.set_charger({"float_voltage": 56})
        with pytest.raises(ValueError):
            await c.set_charger({"charge_current": 71})
        assert not c.api.posts
        async def wrong():
            data = await verified();data["parameters"][0]["verified"] = False;return data
        c.wait_charger = wrong
        with pytest.raises(api.ApiError, match="readback"):
            await c.set_charger({"charge_current": 51})
        assert len(c.api.posts) == 1
    asyncio.run(run())


def test_current_mode_noop_and_advanced_actions_remain_disabled():
    async def run():
        c = new_coordinator()
        await c.set_inverter("inverter_mode", "On")
        await c.set_inverter("ac_input_limit", 50)
        assert not c.api.posts
        with pytest.raises(ValueError):
            await c.set_inverter("ac_input_limit", 50.1)
        with pytest.raises(ValueError):
            await c.set_inverter("equalize", None)
        assert not c.api.posts
    asyncio.run(run())
    assert all(models.BY_KEY[key].disabled for key in models.BUS_ACTIONS)
    assert models.BY_KEY["restart_controller"].disabled


def test_powerassist_fresh_base_masked_write_readback_and_no_retry():
    async def run():
        c = new_coordinator()
        before = {"state": "done", "parameters": [
            {"id": 0, "key": "power_assist", "value": 0, "raw": 33168, "supported": True, "age_s": 0},
            {"id": 48, "key": "boost_factor", "value": 2, "raw": 32, "supported": True,
             "age_s": 0, "minimum": .25, "maximum": 3.5, "step": .0625}]}
        async def read():
            return copy.deepcopy(before)
        c.refresh_assist = read
        async def post(endpoint, fields):
            c.api.posts.append((endpoint, fields.copy()))
            data = copy.deepcopy(before); data["saving"] = True
            for p in data["parameters"]:
                p["verified"] = True; p["value"] = fields[p["key"]]
                p["raw"] = (33168 & ~32) | (32 if fields["power_assist"] else 0) if p["id"] == 0 else round(fields["boost_factor"] * 16)
            c.api.data["assist"] = data
        c.api.post = post
        await c.set_assist({"power_assist": False, "boost_factor": 2})
        assert not c.api.posts
        await c.set_assist({"power_assist": True, "boost_factor": 1.9375})
        assert c.api.posts == [("assist", {"power_assist": 1, "boost_factor": 1.9375, "base": "33168,32"})]
        with pytest.raises(api.ApiError, match="changed while editing"):
            await c.set_assist({"power_assist": True}, base="0,32")
        with pytest.raises(ValueError):
            await c.set_assist({"boost_factor": 1.01})
        assert len(c.api.posts) == 1
        async def wrong(endpoint):
            return {**c.api.data["assist"], "parameters": [{**p, "verified": False} for p in c.api.data["assist"]["parameters"]]}
        c.api.get = wrong
        with pytest.raises(api.ApiError, match="readback"):
            await c.set_assist({"power_assist": True})
        assert len(c.api.posts) == 2
    asyncio.run(run())


def test_controller_restart_requires_identity_new_boot_and_sends_once(monkeypatch):
    async def no_wait(_):
        pass
    monkeypatch.setattr(coord.asyncio, "sleep", no_wait)
    async def run():
        c = new_coordinator(); original = c.api.get; calls = 0
        async def get(endpoint):
            nonlocal calls
            if endpoint != "system":
                return await original(endpoint)
            calls += 1
            return {"controller": c.identity, "boot_id": 1 if calls < 3 else 2, "restart_pending": calls == 2}
        c.api.get = get
        await c.restart_controller()
        assert c.api.posts == [("system", {"reboot": "PRESS"})]
        assert c.api.gets == ["state"]
        c.api.posts.clear()
        async def wrong(_):
            return {"controller": "wrong", "boot_id": 1}
        c.api.get = wrong
        with pytest.raises(api.ApiError, match="identity"):
            await c.restart_controller()
        assert not c.api.posts
    asyncio.run(run())
