#pragma once

#include "protocol.h"
#include "esphome/core/component.h"
#include "esphome/components/canbus/canbus.h"
#include "esphome/components/esp32_ble/ble.h"
#include "esphome/components/esp32_ble_server/ble_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

namespace esphome::pace_can_ble {
class PaceCanBle : public Component {
 public:
  void set_canbus(canbus::Canbus *v) { canbus_ = v; }
  void set_ble(esp32_ble::ESP32BLE *v) { ble_ = v; }
  void set_server(esp32_ble_server::BLEServer *v) { server_ = v; }
  void set_metadata(esp32_ble_server::BLECharacteristic *v) { metadata_ = v; }
  void set_frames(esp32_ble_server::BLECharacteristic *v) { frames_ = v; }
  void setup() override;
  void loop() override;
  void dump_config() override;
  void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);

 private:
  void receive_(uint32_t id, bool extended, bool rtr, const std::vector<uint8_t> &data);
  void metadata_read_();
  canbus::Canbus *canbus_ = nullptr;
  esp32_ble::ESP32BLE *ble_ = nullptr;
  esp32_ble_server::BLEServer *server_ = nullptr;
  esp32_ble_server::BLECharacteristic *metadata_ = nullptr;
  esp32_ble_server::BLECharacteristic *frames_ = nullptr;
  pace_can_ble_protocol::Queue queue_;
  pace_can_ble_protocol::StallGuard stall_;
  portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
  uint32_t boot_ = 0, last_notify_ms_ = 0;
  uint32_t last_diagnostics_ms_ = 0, connect_count_ = 0, disconnect_count_ = 0;
  uint32_t notify_calls_ = 0, metadata_reads_ = 0, advertising_failures_ = 0;
  bool connected_ = false;
  bool advertising_active_ = false;  // Observed events only; never controls advertising.
};
}  // namespace esphome::pace_can_ble
