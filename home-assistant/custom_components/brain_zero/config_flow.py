"""Pair a controller through Home Assistant's MQTT broker."""
import re

import probatio as vol

from homeassistant import config_entries
from homeassistant.core import callback
from homeassistant.helpers import selector

from .api import ApiError, BrainApi
from .models import DOMAIN
from .mqtt_transport import MqttTransport


class BrainZeroConfigFlow(config_entries.ConfigFlow, domain=DOMAIN):
    VERSION = 1

    @staticmethod
    @callback
    def async_get_options_flow(config_entry):
        return BrainZeroOptionsFlow(config_entry)

    async def _verify(self, identity, data):
        transport = MqttTransport(self.hass, identity)
        api = BrainApi(transport.exchange, identity, data["host"].strip(), data.get("port", 80))
        try:
            await transport.start()
            state = await api.get("state")
        finally:
            transport.close()
        reported = state.get("mqtt", {}).get("victron_gx", {}).get("installation_id")
        if reported != identity or "ess" not in state or "ctl" not in state:
            raise ValueError("Not a supported brain_zero controller")
        return {"host": api.host, "port": api.port}

    async def async_step_user(self, user_input=None):
        errors = {}
        if user_input is not None:
            try:
                identity = user_input["controller_id"].strip().lower().replace(":", "")
                if not re.fullmatch(r"[0-9a-f]{12}", identity):
                    raise ValueError("Invalid controller ID")
                data = await self._verify(identity, user_input)
            except (ApiError, OSError, ValueError):
                errors["base"] = "cannot_connect"
            else:
                await self.async_set_unique_id(identity)
                self._abort_if_unique_id_configured()
                return self.async_create_entry(title="brain_zero", data=data)
        return self.async_show_form(step_id="user", data_schema=vol.Schema({
            vol.Required("controller_id"): selector.TextSelector(),
            vol.Required("host", default="brain-zero.local"): selector.TextSelector(),
            vol.Required("port", default=80): int,
        }), errors=errors)

    async def async_step_reconfigure(self, user_input=None):
        entry = self._get_reconfigure_entry()
        errors = {}
        if user_input is not None:
            try:
                data = await self._verify(entry.unique_id, user_input)
            except (ApiError, OSError, ValueError):
                errors["base"] = "cannot_connect"
            if not errors:
                return self.async_update_reload_and_abort(entry, data_updates=data)
        return self.async_show_form(step_id="reconfigure", data_schema=vol.Schema({
            vol.Required("host", default=entry.data.get("host", "brain-zero.local")): selector.TextSelector(),
            vol.Required("port", default=entry.data.get("port", 80)): int,
        }), errors=errors)


class BrainZeroOptionsFlow(config_entries.OptionsFlow):
    def __init__(self, entry):
        self._entry = entry

    async def async_step_init(self, user_input=None):
        errors = {}
        old = dict(self._entry.options)
        if user_input is not None:
            desired = {**old, **user_input}
            for key in ("battery_link_token", "r4875_token"):
                if not user_input.get(key):
                    desired[key] = old.get(key, "")
            for host_key, token_key in (("battery_link_host", "battery_link_token"), ("r4875_host", "r4875_token")):
                host, token = desired.get(host_key, "").strip(), desired.get(token_key, "")
                desired[host_key] = host
                if (host and not re.fullmatch(r"[a-zA-Z0-9][a-zA-Z0-9.-]{0,252}", host) or
                        bool(host) != bool(token) or token and
                        (not 32 <= len(token) <= 128 or any(not 33 <= ord(c) <= 126 for c in token))):
                    errors["base"] = "invalid_charging_options"
            if not errors:
                return self.async_create_entry(title="", data=desired)
        text = selector.TextSelector()
        password = selector.TextSelector({"type": "password"})
        schema = {
            vol.Required("charging_adapter", default=old.get("charging_adapter", False)): selector.BooleanSelector(),
            vol.Optional("battery_link_host", default=old.get("battery_link_host", "")): text,
            vol.Optional("battery_link_token"): password,
            vol.Optional("r4875_host", default=old.get("r4875_host", "")): text,
            vol.Optional("r4875_token"): password,
        }
        return self.async_show_form(step_id="init", data_schema=vol.Schema(schema), errors=errors)
