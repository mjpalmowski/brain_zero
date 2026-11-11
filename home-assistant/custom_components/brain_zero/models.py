"""Known S3 fields and validation. No raw VE.Bus or arbitrary command interface."""
from dataclasses import dataclass
import math
import re

DOMAIN = "brain_zero"
DAYS = ("Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday",
        "Every day", "Weekdays", "Weekends", "First of month")
MODES = ("Optimized with BatteryLife", "Optimized without BatteryLife", "Keep charged", "External control")
INVERTER_MODES = ("Off", "Charger only", "Inverter only", "On")
LIMITS = ("max_charge", "max_discharge", "max_feed_in", "max_import")
RECHARGE_FIELDS = ("enabled", "day", "start", "end", "victron", "r4875", "victron_w", "r4875_w", "victron_soc", "r4875_soc")
CHARGER_FIELDS = ("charge_current", "absorption_voltage", "float_voltage")
CHARGER_SCALES = (1, 100, 100)
BUS_ACTIONS = ("equalize", "stop_equalize_absorption", "stop_equalize_float", "restart_vebus", "redetect_vebus")


@dataclass(frozen=True)
class Spec:
    key: str
    name: str
    kind: str
    source: str
    field: str = ""
    group: str = "controller"
    unit: str | None = None
    device_class: str | None = None
    minimum: float = 0
    maximum: float = 5000
    step: float = 1
    options: tuple[str, ...] = ()
    slot: int = -1
    diagnostic: bool = False
    disabled: bool = False


def build_specs():
    specs = []
    specs.extend((
        Spec("pv_disable", "Disable AC PV", "switch", "pv_disable", "requested", "inverter"),
        Spec("pv_disabled", "PV-disable inverter readback", "binary_sensor", "pv_disable", "disabled", "inverter"),
        Spec("pv_disable_phase", "PV control state", "sensor", "pv_disable", "phase", "inverter"),
        Spec("pv_disable_reason", "PV control reason", "sensor", "pv_disable", "reason", "inverter"),
        Spec("pv_disable_remaining", "PV restoration remaining", "sensor", "pv_disable", "remaining_s", "inverter", "s", "duration"),
        Spec("pv_restore_seconds", "PV automatic restoration duration", "number", "pv_disable", "seconds", "inverter", "s", minimum=10, maximum=300),
    ))
    specs.extend((
        Spec("ignore_ac_input", "Ignore AC input", "switch", "ac_input", "requested", "inverter"),
        Spec("ac_input_ignored", "AC input ignored readback", "binary_sensor", "ac_input", "ignored", "inverter"),
        Spec("grid_accepted", "Grid accepted readback", "binary_sensor", "ac_input", "grid_connected", "inverter"),
        Spec("ac_input_phase", "AC input connection", "sensor", "ac_input", "phase", "inverter"),
        Spec("ac_input_reason", "AC input control reason", "sensor", "ac_input", "reason", "inverter"),
        Spec("ignore_remaining", "Ignore AC remaining", "sensor", "ac_input", "remaining_s", "inverter", "s", "duration"),
        Spec("reconnect_soc", "Grid reconnect SOC", "number", "ac_input", "reconnect_soc", "inverter", "%", minimum=0, maximum=95),
        Spec("reconnect_voltage", "Grid reconnect voltage", "number", "ac_input", "reconnect_v", "inverter", "V", minimum=48, maximum=56, step=.1),
        Spec("ignore_minutes", "Ignore AC duration", "number", "ac_input", "minutes", "inverter", "min", minimum=1, maximum=240),
    ))
    for key, label, field, unit, cls in (
        ("ac_in_voltage", "AC-in voltage", "mains_v", "V", "voltage"),
        ("ac_in_current", "AC-in current", "mains_a", "A", "current"),
        ("ac_in_power", "AC-in power", "acin_w", "W", "power"),
        ("ac_in_frequency", "AC-in frequency", "ac_hz", "Hz", "frequency"),
        ("ac_out_voltage", "AC-out voltage", "inverter_v", "V", "voltage"),
        ("ac_out_current", "AC-out current", "inverter_a", "A", "current"),
        ("ac_out_power", "AC-out power", "acout_w", "W", "power"),
        ("ac_out_frequency", "AC-out frequency", "ac_hz", "Hz", "frequency"),
        ("dc_voltage", "DC voltage", "bat_v", "V", "voltage"),
        ("dc_current", "DC current", "bat_a", "A", "current"),
        ("dc_power", "DC power", "dc_power", "W", "power"),
        ("own_soc", "Inverter state of charge", "soc", "%", "battery"),
    ):
        specs.append(Spec(key, label, "sensor", "state", field, "inverter", unit, cls))
    for field, label, unit, cls in (("battery_soc", "State of charge", "%", "battery"),
                                    ("battery_voltage", "Voltage", "V", "voltage")):
        specs.append(Spec("pace_" + field, label, "sensor", "recharge", field, "battery", unit, cls))
    for key, label, source, field in (
        ("ess_state", "ESS state", "state", "ess.state_name"),
        ("ess_decision", "ESS decision", "state", "ess.why"),
        ("controller_state", "Controller state", "state", "ctl.state"),
        ("charger_stage", "Charger stage", "inverter", "charge_name"),
        ("inverter_readback", "Inverter command readback", "inverter", "command.message"),
        ("last_command", "Last command result", "local", "last_command"),
        ("free_power_phase", "Free-power session state", "free-power", "phase"),
        ("soc_source", "Battery SOC source", "recharge", "victron_soc_source"),
    ):
        specs.append(Spec(key, label, "sensor", source, field, diagnostic=key in ("last_command", "inverter_readback")))
    for key, label, field, unit, cls in (
        ("wifi", "Wi-Fi signal", "rssi", "dBm", "signal_strength"),
        ("uptime", "Uptime", "now_s", "s", "duration"),
        ("heap", "Free memory", "heap", "B", "data_size"),
        ("bus_errors", "Bad bus frames", "bad", None, None),
        ("http_requests", "MQTT requests", "requests", None, None),
        ("http_errors", "MQTT errors", "errors", None, None),
    ):
        specs.append(Spec(key, label, "sensor", "local" if key.startswith("http_") else "state", field,
                          unit=unit, device_class=cls, diagnostic=True))
    for key, label, source, field in (
        ("setpoint_confirmed", "ESS setpoint acknowledged", "state", "setpoint_fresh"),
        ("clock_ready", "Controller clock ready", "state", "ess.clock_valid"),
        ("soc_aligned", "Battery SOC aligned", "recharge", "soc_sync.verified"),
        ("overnight_verified", "Overnight plan verified on both controllers", "recharge", "peer.matching"),
        ("free_power_verified", "Free-power plan verified on both controllers", "free-power", "peer.matching"),
    ):
        specs.append(Spec(key, label, "binary_sensor", source, field))
    for field, name in (("run", "Run local ESS"), ("always_peak_shave", "Peak shaving below minimum SOC"),
                        ("dc_pv_feed_in", "DC solar feed-in"), ("ext_no_charge", "External control: block charging"),
                        ("ext_no_feed_in", "External control: block feed-in")):
        specs.append(Spec(field, name, "switch", "ess", field))
    specs.append(Spec("ess_mode", "ESS mode", "select", "ess", "mode", options=MODES))
    specs.extend((Spec("grid_setpoint", "Grid setpoint", "number", "ess", "grid_setpoint", unit="W", minimum=-5000),
                  Spec("minimum_soc", "Minimum SOC", "number", "ess", "min_soc", unit="%", maximum=100),
                  Spec("safety_cap", "Safety maximum", "number", "ess", "safety_max", unit="W", diagnostic=True)))
    for field, name in zip(LIMITS, ("Charge", "Inverter", "Feed-in", "Grid import"), strict=True):
        specs.append(Spec(field, name + " limit", "number", "ess", field, unit="W"))
        specs.append(Spec(field + "_enabled", name + " limit enabled", "switch", "ess", field))
    specs.append(Spec("inverter_mode", "Operating mode", "select", "inverter", "mode_name", "inverter", options=INVERTER_MODES))
    specs.append(Spec("input_limit", "AC input current limit", "number", "inverter", "current_a", "inverter", "A", "current", maximum=50, step=.1))
    for field, name, unit, maximum, step in (
        ("charge_current", "Stored charge current", "A", 70, 1),
        ("absorption_voltage", "Absorption voltage", "V", 64, .01),
        ("float_voltage", "Float voltage", "V", 64, .01),
    ):
        specs.append(Spec(field, name, "number", "charger", field, "inverter", unit,
                          minimum=0 if field == "charge_current" else 48, maximum=maximum, step=step))
    for slot in range(5):
        for field, label, kind in (("on", "Enabled", "switch"), ("day", "Day", "select"),
                                   ("start", "Start", "time"), ("duration", "Duration", "number"),
                                   ("soc", "Target SOC", "number"), ("discharge", "Allow discharge", "switch")):
            specs.append(Spec(f"schedule_{slot+1}_{field}", f"Schedule {slot+1}: {label}", kind, "schedule", field,
                              unit="min" if field == "duration" else "%" if field == "soc" else None,
                              maximum=1440 if field == "duration" else 100, options=DAYS if field == "day" else (), slot=slot))
    specs.extend((
        Spec("power_assist", "PowerAssist enabled", "switch", "assist", "power_assist", "inverter", diagnostic=True),
        Spec("boost_factor", "PowerAssist boost factor", "number", "assist", "boost_factor", "inverter",
             minimum=.25, maximum=3.5, step=.0625, diagnostic=True),
        Spec("read_assist", "Read PowerAssist settings", "button", "local", "read_assist", "inverter"),
        Spec("restart_controller", "Restart brain_zero controller", "button", "local", "restart_controller", diagnostic=True, disabled=True),
    ))
    for field, label, kind, unit, maximum in (
        ("enabled", "Enabled", "switch", None, 1), ("day", "Day", "select", None, 10),
        ("start", "Start", "time", None, 86399), ("end", "End", "time", None, 86399),
        ("victron", "Use MultiPlus", "switch", None, 1), ("r4875", "Use R4875 pair", "switch", None, 1),
        ("victron_w", "MultiPlus charge power", "number", "W", 5000),
        ("r4875_w", "R4875 charge power", "number", "W", 10000),
        ("victron_soc", "MultiPlus target SOC", "number", "%", 100),
        ("r4875_soc", "R4875 target SOC", "number", "%", 100),
    ):
        specs.append(Spec("overnight_" + field, "Overnight: " + label, kind, "recharge", field,
                          unit=unit, maximum=maximum, options=DAYS if field == "day" else ()))
    for field, label, kind in (("start", "Start", "datetime"), ("end", "End", "datetime"),
                               ("charge_w", "MultiPlus charge power", "number"), ("chargers", "Include R4875 pair", "switch")):
        specs.append(Spec("free_power_" + field, "Free power draft: " + label, kind, "draft", field,
                          unit="W" if kind == "number" else None))
    for key, label in (("refresh_charger", "Read stored charger settings"), ("refresh_settings", "Refresh settings"),
                       ("arm_free_power", "Arm saved free-power draft"), ("cancel_free_power", "Cancel free-power session"),
                       ("reload_free_power", "Reload free-power draft")):
        specs.append(Spec(key, label, "button", "local", key, group="inverter" if key == "refresh_charger" else "controller"))
    for key, label in zip(BUS_ACTIONS, ("Start equalization", "Interrupt equalization to absorption", "Interrupt equalization to float",
                                      "Restart VE.Bus system", "Redetect VE.Bus system"), strict=True):
        specs.append(Spec(key, label, "button", "inverter", key, "inverter", diagnostic=True, disabled=True))
    return tuple(specs)


SPECS = build_specs()
BY_KEY = {s.key: s for s in SPECS}


def number(value, lo, hi, step=1):
    if isinstance(value, bool):
        raise ValueError("A number is required")
    try:
        value = float(value)
    except (TypeError, ValueError) as err:
        raise ValueError("A finite number is required") from err
    if not math.isfinite(value) or not lo <= value <= hi or abs(value / step - round(value / step)) > 1e-6:
        raise ValueError(f"Use {lo} to {hi}, in steps of {step}")
    return round(value, 6)


def flag(value):
    if value not in (False, True, 0, 1):
        raise ValueError("An enabled/disabled value is required")
    return int(bool(value))


def seconds(value):
    if isinstance(value, str) and ":" in value:
        parts = value.split(":")
        if len(parts) not in (2, 3) or not all(re.fullmatch(r"\d{1,2}", p) for p in parts):
            raise ValueError("Use HH:MM or HH:MM:SS")
        h, m = int(parts[0]), int(parts[1]); s = int(parts[2]) if len(parts) == 3 else 0
        if h > 23 or m > 59 or s > 59:
            raise ValueError("Invalid clock time")
        return h * 3600 + m * 60 + s
    return int(number(value, 0, 86399))


def validate_recharge(data):
    out = {key: data[key] for key in RECHARGE_FIELDS}
    for key in ("enabled", "victron", "r4875"):
        out[key] = flag(out[key])
    out["day"] = int(number(out["day"], 0, 10))
    out["start"], out["end"] = seconds(out["start"]), seconds(out["end"])
    for key, hi in (("victron_w", 5000), ("r4875_w", 10000), ("victron_soc", 100), ("r4875_soc", 100)):
        out[key] = int(number(out[key], 0, hi))
    if out["start"] == out["end"] or (out["enabled"] and not (out["victron"] or out["r4875"])):
        raise ValueError("Choose different start/end times and at least one charger when enabled")
    return out


def validate_schedule(row):
    out = {"on": flag(row["on"]), "day": int(number(row["day"], 0, 10)), "start": seconds(row["start"]),
           "duration": int(number(row["duration"], 0, 86400)), "soc": int(number(row["soc"], 0, 100)),
           "discharge": flag(row["discharge"])}
    if out["on"] and not out["duration"]:
        raise ValueError("An enabled schedule needs a nonzero duration")
    return out


def path(data, key):
    for part in key.split("."):
        if not isinstance(data, dict):
            return None
        data = data.get(part)
    return data


def live_reading(state, key, elapsed=0):
    bus_age = state.get("multi_age_s")
    sample = state.get(key)
    if bus_age is None or bus_age + elapsed >= 5 or not isinstance(sample, list) or len(sample) != 2:
        return None
    v, age = sample
    return v if isinstance(v, (int, float)) and math.isfinite(v) and age is not None and 0 <= age + elapsed < 10 else None
