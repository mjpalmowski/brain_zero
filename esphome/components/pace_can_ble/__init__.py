"""Read-only CAN receive transport for brain_zero's BLE battery input."""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import canbus, esp32_ble, esp32_ble_server
import esphome.final_validate as fv
from esphome.const import CONF_ID

DEPENDENCIES = ["canbus", "esp32_ble", "esp32_ble_server"]
namespace = cg.esphome_ns.namespace("pace_can_ble")
PaceCanBle = namespace.class_("PaceCanBle", cg.Component)

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(PaceCanBle),
    cv.GenerateID(esp32_ble.CONF_BLE_ID): cv.use_id(esp32_ble.ESP32BLE),
    cv.Required("canbus_id"): cv.use_id(canbus.CanbusComponent),
    cv.Required("server_id"): cv.use_id(esp32_ble_server.BLEServer),
    cv.Required("metadata_id"): cv.use_id(esp32_ble_server.BLECharacteristic),
    cv.Required("frames_id"): cv.use_id(esp32_ble_server.BLECharacteristic),
}).extend(cv.COMPONENT_SCHEMA)


def validate_receive_queue(config):
    full = fv.full_config.get()
    expected = str(config["canbus_id"])
    target = next((v for v in full.get("canbus", []) if str(v[CONF_ID]) == expected), None)
    if target is None or target.get("platform") != "esp32_can":
        raise cv.Invalid("pace_can_ble requires the existing esp32_can receiver")
    # TWAI_GENERAL_CONFIG_DEFAULT uses five RX slots in the pinned ESP-IDF.
    # A complete queued backlog must drain in one CAN loop after a detected stall.
    if target.get("rx_queue_len", 5) > target.get("max_frames_per_loop", 50):
        raise cv.Invalid("CAN rx_queue_len must not exceed max_frames_per_loop for timestamp quarantine")
    server = full.get("esp32_ble_server", {})
    if str(server.get(CONF_ID, "")) != str(config["server_id"]) or server.get("max_clients", 1) != 1:
        raise cv.Invalid("pace_can_ble requires one BLE central (esp32_ble_server max_clients: 1)")
    return config


FINAL_VALIDATE_SCHEMA = validate_receive_queue


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    parent = await cg.get_variable(config[esp32_ble.CONF_BLE_ID])
    cg.add(var.set_ble(parent))
    esp32_ble.register_gap_event_handler(parent, var)
    cg.add(var.set_canbus(await cg.get_variable(config["canbus_id"])))
    cg.add(var.set_server(await cg.get_variable(config["server_id"])))
    cg.add(var.set_metadata(await cg.get_variable(config["metadata_id"])))
    cg.add(var.set_frames(await cg.get_variable(config["frames_id"])))
