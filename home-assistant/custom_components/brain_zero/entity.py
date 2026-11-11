"""All entities share one coordinator and never poll the S3 independently."""
from datetime import UTC, datetime, time
from time import monotonic

from homeassistant.components.binary_sensor import BinarySensorEntity
from homeassistant.components.button import ButtonEntity
from homeassistant.components.datetime import DateTimeEntity
from homeassistant.components.number import NumberEntity, NumberMode
from homeassistant.components.select import SelectEntity
from homeassistant.components.sensor import SensorEntity, SensorStateClass
from homeassistant.components.switch import SwitchEntity
from homeassistant.components.time import TimeEntity
from homeassistant.helpers.device_registry import DeviceInfo
from homeassistant.helpers.entity import EntityCategory
from homeassistant.helpers.update_coordinator import CoordinatorEntity

from .models import DOMAIN, LIMITS, SPECS


class BrainEntity(CoordinatorEntity):
    _attr_has_entity_name = True

    def __init__(self, coordinator, spec):
        super().__init__(coordinator)
        self.spec = spec
        self._attr_unique_id = f"{coordinator.identity}_{spec.key}"
        self._attr_name = spec.name
        self._attr_entity_registry_enabled_default = not spec.disabled
        if spec.diagnostic:
            self._attr_entity_category = EntityCategory.DIAGNOSTIC if spec.kind in ("sensor", "binary_sensor") else EntityCategory.CONFIG
        group = spec.group
        names = {"controller": "brain_zero", "inverter": "brain_zero MultiPlus-II", "battery": "brain_zero PACE battery"}
        self._attr_device_info = DeviceInfo(
            identifiers={(DOMAIN, f"{coordinator.identity}_{group}")},
            name=names[group], manufacturer={"controller": "mjpalmowski", "inverter": "Victron Energy", "battery": "PACE"}[group],
            model={"controller": "brain_zero S3", "inverter": "MultiPlus-II 48/5000/70-50", "battery": "PACE CAN battery monitor"}[group],
            configuration_url=coordinator.api.url + ("/inverter" if group == "inverter" else "/"),
            **({"serial_number": coordinator.identity} if group == "controller" else {"via_device_id": coordinator.controller_device_id}),
        )

    @property
    def available(self):
        return super().available and self.coordinator.available(self.spec)

    @property
    def extra_state_attributes(self):
        c, spec = self.coordinator, self.spec
        attrs = {}
        if spec.source == "draft":
            attrs.update({"draft_only": True, "apply_with": "Arm saved free-power draft", "base_revision": c.draft_base,
                          "saved_value": c.snapshot.get("free-power", {}).get(spec.field)})
        if spec.source in ("charger", "assist"):
            p = next((p for p in c.snapshot.get(spec.source, {}).get("parameters", []) if p.get("key") == spec.field), {})
            age = p.get("age_s", -1)
            attrs.update({"stored_readback": True, "last_read_age_s": round(age + monotonic() - c.fetched.get(spec.source, monotonic()), 1) if age >= 0 else None,
                          "refresh_on_write": True})
        if spec.source == "ess" and spec.field in LIMITS:
            attrs["limit_enabled"] = c.snapshot.get("ess", {}).get("settings", {}).get(spec.field, -1) >= 0
            attrs["preset_only_when_disabled"] = True
        return attrs or None


class BrainSensor(BrainEntity, SensorEntity):
    def __init__(self, coordinator, spec):
        super().__init__(coordinator, spec)
        self._attr_native_unit_of_measurement = spec.unit
        self._attr_device_class = spec.device_class
        if spec.unit and spec.key != "uptime":
            self._attr_state_class = SensorStateClass.MEASUREMENT
        if spec.unit in ("W", "V", "A", "%", "Hz"):
            self._attr_suggested_display_precision = 1 if spec.unit != "Hz" else 2

    @property
    def native_value(self):
        return self.coordinator.value(self.spec)


class BrainBinarySensor(BrainEntity, BinarySensorEntity):
    @property
    def is_on(self):
        value = self.coordinator.value(self.spec)
        return bool(value) if value is not None else None


class BrainNumber(BrainEntity, NumberEntity):
    def __init__(self, coordinator, spec):
        super().__init__(coordinator, spec)
        self._attr_native_min_value, self._attr_native_max_value = spec.minimum, spec.maximum
        self._attr_native_step = spec.step
        self._attr_native_unit_of_measurement = spec.unit
        self._attr_device_class = spec.device_class
        self._attr_mode = NumberMode.BOX

    @property
    def native_value(self):
        return self.coordinator.value(self.spec)

    async def async_set_native_value(self, value):
        await self.coordinator.apply(self.spec, value)


class BrainSwitch(BrainEntity, SwitchEntity):
    @property
    def is_on(self):
        value = self.coordinator.value(self.spec)
        return bool(value) if value is not None else None

    async def async_turn_on(self, **kwargs):
        await self.coordinator.apply(self.spec, True)

    async def async_turn_off(self, **kwargs):
        await self.coordinator.apply(self.spec, False)


class BrainSelect(BrainEntity, SelectEntity):
    def __init__(self, coordinator, spec):
        super().__init__(coordinator, spec)
        self._attr_options = list(spec.options)

    @property
    def current_option(self):
        return self.coordinator.value(self.spec)

    async def async_select_option(self, option):
        await self.coordinator.apply(self.spec, option)


class BrainTime(BrainEntity, TimeEntity):
    @property
    def native_value(self):
        value = self.coordinator.value(self.spec)
        return None if value is None else time(int(value) // 3600, int(value) // 60 % 60, int(value) % 60)

    async def async_set_value(self, value):
        await self.coordinator.apply(self.spec, value.hour * 3600 + value.minute * 60 + value.second)


class BrainDateTime(BrainEntity, DateTimeEntity):
    @property
    def native_value(self):
        value = self.coordinator.value(self.spec)
        return datetime.fromtimestamp(value, UTC) if value and value >= 1767225600 else None

    @property
    def available(self):
        # An unset draft must remain editable so the user can supply the dates.
        return self.coordinator.last_update_success

    async def async_set_value(self, value):
        if value.tzinfo is None:
            from homeassistant.exceptions import HomeAssistantError
            raise HomeAssistantError("Supply a date/time with a time zone")
        await self.coordinator.apply(self.spec, int(value.timestamp()))


class BrainButton(BrainEntity, ButtonEntity):
    async def async_press(self):
        await self.coordinator.apply(self.spec)


ENTITY_TYPES = {"sensor": BrainSensor, "binary_sensor": BrainBinarySensor, "number": BrainNumber,
                "switch": BrainSwitch, "select": BrainSelect, "time": BrainTime,
                "datetime": BrainDateTime, "button": BrainButton}


def entities(coordinator, kind):
    return [ENTITY_TYPES[kind](coordinator, spec) for spec in SPECS if spec.kind == kind]
