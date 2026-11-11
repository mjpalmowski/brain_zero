"""Local brain_zero control over Home Assistant's MQTT broker."""
from datetime import datetime
import logging

import probatio as vol

from homeassistant.const import Platform
from homeassistant.core import SupportsResponse
from homeassistant.exceptions import ConfigEntryNotReady, HomeAssistantError
from homeassistant.helpers import device_registry as dr

from .api import ApiError, BrainApi
from .coordinator import BrainCoordinator, PERIODS
from .charging import ChargingAdapter, OPTIONS as CHARGING_OPTIONS
from .models import DOMAIN
from .mqtt_transport import MqttTransport

PLATFORMS = [Platform.SENSOR, Platform.BINARY_SENSOR, Platform.NUMBER, Platform.SWITCH,
             Platform.SELECT, Platform.TIME, Platform.DATETIME, Platform.BUTTON]
_LOGGER = logging.getLogger(__name__)
SERVICE_FIELDS = {
    "set_pv_disable": {"disable": bool, "seconds": int, "base": int},
    "set_ac_input": {"ignore": bool, "reconnect_soc": float, "reconnect_v": float, "minutes": int, "base": int},
    "set_ess": {"run": bool, "mode": int, "min_soc": float, "grid_setpoint": int, "max_charge": int,
                "max_discharge": int, "max_feed_in": int, "max_import": int, "always_peak_shave": bool,
                "dc_pv_feed_in": bool, "ext_no_charge": bool, "ext_no_feed_in": bool, "safety_max": int},
    "set_schedule": {"slot": int, "on": bool, "day": int, "start": str, "duration": int, "soc": int, "discharge": bool},
    "set_schedules": {"windows": list, "base": list},
    "set_overnight": {"enabled": bool, "day": int, "start": str, "end": str, "victron": bool, "r4875": bool,
                      "victron_w": int, "r4875_w": int, "victron_soc": int, "r4875_soc": int, "base_id": int},
    "set_charger": {"charge_current": float, "absorption_voltage": float, "float_voltage": float, "base": str},
    "set_assist": {"power_assist": bool, "boost_factor": float, "base": str},
    "set_free_power": {"start": str, "end": str, "charge_w": int, "chargers": bool},
    "cancel_free_power": {},
    "refresh": {"section": str},
    "get_snapshot": {},
    "restart_controller": {},
}


def register_services(hass):
    async def handle(call):
        args = dict(call.data)
        entries = hass.data.get(DOMAIN, {})
        entry_id = args.pop("entry_id", None)
        if entry_id is None and len(entries) == 1:
            entry_id = next(iter(entries))
        c = entries.get(entry_id)
        if c is None:
            raise HomeAssistantError("Choose a loaded brain_zero config entry")
        if call.service == "get_snapshot":
            return c.cached_snapshot()

        async def action():
            if call.service == "set_pv_disable":
                await c.set_pv_disable(args, base=args.pop("base", None))
            elif call.service == "set_ac_input":
                await c.set_ac_input(args, base=args.pop("base", None))
            elif call.service == "set_ess":
                await c.set_ess(args)
            elif call.service == "set_schedule":
                if "slot" not in args:
                    raise ValueError("Choose a schedule slot from 1 to 5")
                slot = args.pop("slot")
                await c.set_schedule(slot, args)
            elif call.service == "set_overnight":
                await c.set_overnight(args, expected_id=args.pop("base_id", None))
            elif call.service == "set_schedules":
                await c.set_schedules(args.get("windows"), args.get("base"))
            elif call.service == "set_charger":
                await c.set_charger(args, base=args.pop("base", None))
            elif call.service == "set_assist":
                await c.set_assist(args, base=args.pop("base", None))
            elif call.service == "set_free_power":
                fields = dict(args)
                for key in ("start", "end"):
                    if key not in fields:
                        raise ValueError("Supply both start and end dates with a timezone offset")
                    dt = datetime.fromisoformat(fields[key])
                    if dt.tzinfo is None:
                        raise ValueError("Supply a timezone offset, such as +00:00 or +01:00")
                    fields[key] = int(dt.timestamp())
                await c.free_power(fields=fields)
            elif call.service == "cancel_free_power":
                await c.free_power(cancel=True)
            elif call.service == "restart_controller":
                await c.restart_controller()
            elif call.service == "refresh":
                section = args.get("section")
                if section == "charger":
                    await c.refresh_charger()
                    return
                if section == "assist":
                    await c.refresh_assist()
                    return
                if section is not None and section not in (*PERIODS, "state"):
                    raise ValueError("Choose a supported settings section")
                for endpoint in (section,) if section else PERIODS:
                    await c.fetch(endpoint)
        await c.execute(call.service.replace("_", " "), action)
        return {**c.cached_snapshot(), "result": c.last_command}

    for name, fields in SERVICE_FIELDS.items():
        schema = {vol.Optional("entry_id"): str}
        schema.update({vol.Optional(key): vol.Coerce(float) if cls in (float, int) else cls for key, cls in fields.items()})
        hass.services.async_register(DOMAIN, name, handle, schema=vol.Schema(schema), supports_response=SupportsResponse.OPTIONAL)


async def async_setup_entry(hass, entry):
    transport = MqttTransport(hass, entry.unique_id)
    api = BrainApi(transport.exchange, entry.unique_id, entry.data.get("host", "brain-zero.local"),
                   entry.data.get("port", 80))
    try:
        await transport.start()
    except OSError as err:
        raise ConfigEntryNotReady(str(err)) from err
    entry.async_on_unload(transport.close)
    c = BrainCoordinator(hass, entry, api)
    c.charging = ChargingAdapter(c, transport)
    await c.charging.start()
    await c.async_config_entry_first_refresh()
    # One initial read of the three stored parameters; never an idle read loop.
    try:
        await c.execute("Initial charger readback", c.refresh_charger)
    except HomeAssistantError as err:
        _LOGGER.warning("Charger readback is pending; use Read stored charger settings: %s", err)
    try:
        await c.execute("Initial PowerAssist readback", c.refresh_assist)
    except HomeAssistantError as err:
        _LOGGER.warning("PowerAssist readback is pending; use Read PowerAssist settings: %s", err)
    try:
        await c.fetch("state")
        c.publish()
    except ApiError as err:
        raise ConfigEntryNotReady("Waiting for fresh controller telemetry") from err
    entry.runtime_data = c
    async def options_updated(hass, updated):
        if any(updated.options.get(key) != c.charging.options.get(key) for key in CHARGING_OPTIONS):
            await hass.config_entries.async_reload(updated.entry_id)
    entry.async_on_unload(entry.add_update_listener(options_updated))
    controller = dr.async_get(hass).async_get_or_create(
        config_entry_id=entry.entry_id, identifiers={(DOMAIN, f"{c.identity}_controller")},
        name="brain_zero", manufacturer="mjpalmowski", model="brain_zero S3",
        serial_number=c.identity, configuration_url=api.url + "/",
    )
    c.controller_device_id = controller.id
    if DOMAIN not in hass.data:
        hass.data[DOMAIN] = {}
        register_services(hass)
    hass.data[DOMAIN][entry.entry_id] = c
    await hass.config_entries.async_forward_entry_setups(entry, PLATFORMS)
    if "api_token" in entry.data:
        data = {key: value for key, value in entry.data.items() if key != "api_token"}
        hass.config_entries.async_update_entry(entry, data=data)
    return True


async def async_unload_entry(hass, entry):
    if not await hass.config_entries.async_unload_platforms(entry, PLATFORMS):
        return False
    hass.data[DOMAIN].pop(entry.entry_id, None)
    if not hass.data[DOMAIN]:
        for name in SERVICE_FIELDS:
            hass.services.async_remove(DOMAIN, name)
        hass.data.pop(DOMAIN)
    return True
