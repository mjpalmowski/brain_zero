"""Sensor platform; updates come only from the shared coordinator."""
from .entity import entities


async def async_setup_entry(hass, entry, async_add_entities):
    async_add_entities(entities(entry.runtime_data, "sensor"))
