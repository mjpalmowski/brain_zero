#include "pace_can_ble.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "esp_random.h"
#include <algorithm>

namespace esphome::pace_can_ble {
static const char *const TAG = "pace_can_ble";
static constexpr uint32_t NOTIFY_INTERVAL_MS = 25;

void PaceCanBle::setup() {
  boot_ = esp_random();
  if (!boot_) boot_ = 1;
  stall_.begin(millis());
  metadata_->get_value().resize(pace_can_ble_protocol::RECORD_SIZE);
  frames_->get_value().resize(pace_can_ble_protocol::RECORD_SIZE);
  metadata_->on_read([this](uint16_t) {
    ++this->metadata_reads_;
    this->metadata_read_();
  });
  server_->on_connect([this](uint16_t) {
    portENTER_CRITICAL(&this->mux_);
    this->queue_.clear();
    this->connected_ = true;
    this->advertising_active_ = false;  // A connection ends connectable advertising.
    portEXIT_CRITICAL(&this->mux_);
    ++this->connect_count_;
    ESP_LOGI(TAG, "BLE connected; clients=%u", unsigned(this->server_->get_connected_client_count()));
  });
  server_->on_disconnect([this](uint16_t) {
    portENTER_CRITICAL(&this->mux_);
    this->connected_ = false;
    this->queue_.clear();
    portEXIT_CRITICAL(&this->mux_);
    ++this->disconnect_count_;
    ESP_LOGI(TAG, "BLE disconnected; clients=%u", unsigned(this->server_->get_connected_client_count()));
  });
  canbus_->add_callback([this](uint32_t id, bool extended, bool rtr, const std::vector<uint8_t> &data) {
    this->receive_(id, extended, rtr, data);
  });
  metadata_read_();
  ESP_LOGI(TAG, "PACE receive-only BLE diagnostics v1 enabled");
}

void PaceCanBle::gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
  // Observe asynchronous completion only. Advertising remains owned by the
  // ESPHome server, including its one-client limit and disconnect restart.
  const char *operation = nullptr;
  esp_bt_status_t status = ESP_BT_STATUS_SUCCESS;
  switch (event) {
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
      operation = "start";
      status = param->adv_start_cmpl.status;
      advertising_active_ = status == ESP_BT_STATUS_SUCCESS;
      break;
    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
      operation = "stop";
      status = param->adv_stop_cmpl.status;
      if (status == ESP_BT_STATUS_SUCCESS) advertising_active_ = false;
      break;
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
      operation = "data";
      status = param->adv_data_cmpl.status;
      break;
    case ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT:
      operation = "scan response";
      status = param->scan_rsp_data_cmpl.status;
      break;
    default:
      return;
  }
  if (status == ESP_BT_STATUS_SUCCESS) {
    ESP_LOGI(TAG, "BLE advertising %s complete; status=%u", operation, unsigned(status));
  } else {
    ++advertising_failures_;
    ESP_LOGW(TAG, "BLE advertising %s failed; status=%u", operation, unsigned(status));
  }
}

void PaceCanBle::receive_(uint32_t id, bool extended, bool rtr, const std::vector<uint8_t> &data) {
  const uint32_t now = millis();
  portENTER_CRITICAL(&mux_);
  const bool timely = stall_.observe(now);
  if (!timely) queue_.clear();
  const bool usable = connected_ && timely;
  if (queue_.receive(id, extended, rtr, data.data(), data.size(), now, usable) && !timely)
    ++queue_.dropped;
  portEXIT_CRITICAL(&mux_);
}

void PaceCanBle::metadata_read_() {
  // Called by the BLE read handler. Source time must be generated for this read,
  // never copied from a setup-time value or a periodic notification cache.
  portENTER_CRITICAL(&mux_);
  const auto value = pace_can_ble_protocol::metadata(boot_, millis(), queue_.received, queue_.dropped);
  portEXIT_CRITICAL(&mux_);
  auto &dest = metadata_->get_value();
  std::copy(value.begin(), value.end(), dest.begin());
}

void PaceCanBle::loop() {
  const uint32_t now = millis();
  pace_can_ble_protocol::Record record{};
  bool ready = false;
  portENTER_CRITICAL(&mux_);
  const bool timely = stall_.loop(now);
  if (!timely) queue_.clear();
  if (connected_ && timely && uint32_t(now - last_notify_ms_) >= NOTIFY_INTERVAL_MS) {
    ready = queue_.pop(record);
    if (ready) last_notify_ms_ = now;
  }
  portEXIT_CRITICAL(&mux_);
  if (uint32_t(now - last_diagnostics_ms_) >= 30000) {
    last_diagnostics_ms_ = now;
    portENTER_CRITICAL(&mux_);
    const uint32_t received = queue_.received, dropped = queue_.dropped;
    const bool connected = connected_;
    portEXIT_CRITICAL(&mux_);
    ESP_LOGI(TAG,
             "BLE status: active=%u server=%u clients=%u link=%u adv=%u connects=%u disconnects=%u rx=%u dropped=%u notify_calls=%u reads=%u adv_failures=%u",
             unsigned(ble_->is_active()), unsigned(server_->is_running()), unsigned(server_->get_connected_client_count()),
             unsigned(connected), unsigned(advertising_active_), unsigned(connect_count_), unsigned(disconnect_count_),
             unsigned(received), unsigned(dropped), unsigned(notify_calls_), unsigned(metadata_reads_),
             unsigned(advertising_failures_));
  }
  if (!ready || !frames_->is_created()) return;
  auto &dest = frames_->get_value();
  std::copy(record.begin(), record.end(), dest.begin());
  ++notify_calls_;
  frames_->notify();
}

void PaceCanBle::dump_config() {
  ESP_LOGCONFIG(TAG, "PACE CAN-to-BLE receive-only transport v1");
  ESP_LOGCONFIG(TAG, "  Seven allowlisted CAN IDs; 20-byte BLE records; no CAN transmit endpoint");
  ESP_LOGCONFIG(TAG, "  Wi-Fi and OTA configuration are retained by the parent device");
}
}  // namespace esphome::pace_can_ble
