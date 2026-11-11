"""Use Home Assistant's existing MQTT connection for controller requests."""
import asyncio
import json

from homeassistant.components import mqtt
from homeassistant.components.mqtt.client import async_on_subscribe_done
from homeassistant.core import callback
from homeassistant.exceptions import HomeAssistantError


class MqttTransport:
    def __init__(self, hass, identity):
        self.hass = hass
        self.topic = f"brain_zero/{identity}/reply/+"
        self.unsubscribe = None
        self.pending = {}

    async def start(self):
        try:
            async with asyncio.timeout(10):
                if not await mqtt.async_wait_for_mqtt_client(self.hass):
                    raise OSError("Set up the MQTT integration first")
                self.unsubscribe = await mqtt.async_subscribe(self.hass, self.topic, self.receive, qos=1, encoding=None)
                ready = asyncio.Event()
                stop_waiting = async_on_subscribe_done(self.hass, self.topic, 1, ready.set)
                try:
                    await ready.wait()
                finally:
                    stop_waiting()
        except (TimeoutError, OSError, HomeAssistantError) as err:
            self.close()
            raise OSError("Home Assistant MQTT connection or reply subscription unavailable") from err

    @callback
    def receive(self, message):
        waiting = self.pending.get(message.topic)
        if not message.retain and waiting is not None and not waiting.done():
            waiting.set_result(message.payload)

    async def exchange(self, request_topic, reply_topic, payload):
        if not mqtt.is_connected(self.hass):
            raise OSError("Home Assistant MQTT is disconnected")
        waiting = asyncio.get_running_loop().create_future()
        self.pending[reply_topic] = waiting
        try:
            # A lost command is reported to the caller. There is no broker retry.
            await mqtt.async_publish(self.hass, request_topic, payload, qos=0, retain=False)
            return await waiting
        except HomeAssistantError as err:
            raise OSError("Home Assistant could not publish the controller request") from err
        finally:
            self.pending.pop(reply_topic, None)

    async def publish_external(self, payload, topic):
        """Measurements never use the RPC request/reply queue or retained MQTT."""
        if not mqtt.is_connected(self.hass):
            raise OSError("Home Assistant MQTT is disconnected")
        raw = json.dumps(payload, separators=(",", ":"), allow_nan=False)
        if len(raw.encode()) > 256:
            raise ValueError("External battery sample exceeds the controller limit")
        try:
            await mqtt.async_publish(self.hass, topic, raw, qos=0, retain=False)
        except HomeAssistantError as err:
            raise OSError("Home Assistant could not publish the battery sample") from err

    @callback
    def close(self):
        if self.unsubscribe is not None:
            self.unsubscribe()
            self.unsubscribe = None
        for waiting in self.pending.values():
            waiting.cancel()
        self.pending.clear()
