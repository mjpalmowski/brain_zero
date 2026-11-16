#include "battery_ble.h"
#include "battery_ble_queue.h"

#include <ctype.h>
#include <math.h>
#include <string.h>

namespace battery_ble {

namespace {
bool address(char (&value)[18]) {
    if (!memchr(value, 0, sizeof value)) return false;
    if (!value[0]) return true;
    if (strlen(value) != 17) return false;
    bool zero = true, broadcast = true;
    for (unsigned i = 0; i < 17; ++i) {
        if (i % 3 == 2) { if (value[i] != ':') return false; continue; }
        const unsigned char c = static_cast<unsigned char>(value[i]);
        if (!isxdigit(c)) return false;
        value[i] = char(toupper(c));
        zero &= value[i] == '0'; broadcast &= value[i] == 'F';
    }
    return !zero && !broadcast;
}
}

bool normalize(Settings &s, const char **why) {
    const char *error = nullptr;
    if (s.version != 1) error = "Unsupported Bluetooth settings version";
    else if (!address(s.jk_address) || !address(s.pace_address))
        error = "Bluetooth addresses must use AA:BB:CC:DD:EE:FF";
    else if (s.jk_address[0] && !strcmp(s.jk_address, s.pace_address))
        error = "Choose a different Bluetooth device for each battery";
    else if (!isfinite(s.jk_capacity_ah) || !isfinite(s.pace_capacity_ah) ||
             s.jk_capacity_ah < 0 || s.pace_capacity_ah < 0 ||
             s.jk_capacity_ah > 10000 || s.pace_capacity_ah > 10000)
        error = "Battery capacity must be 0 to 10000 Ah";
    else if (s.enabled && !s.jk_address[0] && !s.pace_address[0])
        error = "Set at least one Bluetooth battery address";
    if (why) *why = error;
    return !error;
}

const char *state_name(State s) {
    switch (s) {
    case State::OFF: return "disabled";
    case State::STARTING: return "starting";
    case State::WAITING: return "waiting";
    case State::CONNECTING: return "connecting";
    case State::DISCOVERING: return "discovering";
    case State::IDENTIFYING: return "identifying";
    case State::CLOCK_WAIT: return "clock_wait";
    case State::STREAMING: return "streaming";
    case State::BACKOFF: return "backoff";
    case State::NO_MEMORY: return "no_memory";
    case State::STACK_FAILED: return "stack_failed";
    case State::DEFERRED: return "deferred";
    case State::CLOSING: return "closing";
    }
    return "unknown";
}

}  // namespace battery_ble

#if defined(ARDUINO_ARCH_ESP32)
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <NimBLEClient.h>
#include <NimBLERemoteService.h>
#include <NimBLERemoteCharacteristic.h>
#include <NimBLEAttValue.h>
#include "nimble/nimble/host/include/host/ble_gap.h"
#include <WiFi.h>
#include <atomic>
#include <new>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/timers.h"

#ifndef BRAIN_ZERO_BLE_BOOT_RECOVERY
#define BRAIN_ZERO_BLE_BOOT_RECOVERY 0
#endif

namespace battery_ble {
namespace {

constexpr char PACE_SERVICE[] = "e6c00001-b5a3-4f4f-9b8d-73d619b32720";
constexpr char PACE_METADATA[] = "e6c00002-b5a3-4f4f-9b8d-73d619b32720";
constexpr char PACE_FRAMES[] = "e6c00003-b5a3-4f4f-9b8d-73d619b32720";
constexpr size_t MAX_NOTIFICATION = detail::NOTIFICATION_BYTES;
constexpr uint32_t OPERATION_MS = 5500, RETRY_MAX_MS = 30000;
constexpr uint32_t CONNECT_TIMEOUT_MS = 10000, CONNECT_GUARD_MS = 12000;
constexpr uint32_t CLOSE_GUARD_MS = 3000, CLOSE_QUIET_MS = 40;
constexpr int MEMORY_ERROR = -1001, SERVICE_ERROR = -1002, PROPERTIES_ERROR = -1003;
constexpr int SUBSCRIBE_ERROR = -1004, METADATA_ERROR = -1005, IDENTITY_TIMEOUT = -1006;
constexpr int READ_REQUEST_ERROR = -1007, STREAM_TIMEOUT = -1008;
constexpr int NOTIFICATION_OVERFLOW = -1009;
constexpr int ADAPTER_RESTARTED = -1010;
constexpr int WIFI_COEX_ERROR = -1012;
constexpr int GAP_TIMEOUT_ERROR = -1013, CONFIG_CHANGED_ERROR = -1014;

uint32_t operation_deadline(uint32_t duration = OPERATION_MS) {
    const uint32_t deadline = millis() + duration;
    return deadline ? deadline : 1;  // Zero denotes no operation, including millis wrap.
}

void wipe(void *p, size_t length) {
    detail::secure_zero(p, length);
}

using Message = detail::Notification;

struct Runtime;
struct Link;
struct Callbacks final : NimBLEClientCallbacks {
    explicit Callbacks(Link *l) : link(l) {}
    Link *link;
    void onConnect(NimBLEClient *) override;
    void onConnectFail(NimBLEClient *, int reason) override;
    void onDisconnect(NimBLEClient *, int reason) override;
};

struct Link {
    Runtime *owner = nullptr;
    unsigned index = 0;
    NimBLEClient *client = nullptr;  // Lifetime is the initialized host lifetime.
    NimBLERemoteCharacteristic *notify = nullptr, *write = nullptr, *metadata = nullptr;
    Callbacks callbacks{this};
    std::atomic<TaskHandle_t> task{nullptr};
    SemaphoreHandle_t feed_lock = nullptr;
    Message *queue = nullptr;
    uint8_t capacity = 0;
    detail::NotificationQueue pending;
    portMUX_TYPE queue_lock = portMUX_INITIALIZER_UNLOCKED;
    std::atomic<bool> connected{false};
    std::atomic<uint32_t> epoch{1}, deadline_ms{0}, attempts{0}, connections{0};
    std::atomic<uint32_t> configuration_revision{0};
    std::atomic<uint32_t> notifications{0}, dropped{0}, last_notification_ms{0};
    std::atomic<uint32_t> gap_events{0};
    std::atomic<bool> queue_problem{false};
    std::atomic<int> last_error{0}, rssi{0};
    std::atomic<State> state{State::STARTING};
    std::atomic<uint32_t> next_retry_ms{0};
    uint32_t backoff_ms = 1000;
    char active_address[18] = {};
    uint32_t connected_ms = 0, request_ms = 0, metadata_ms = 0;
    uint8_t identity_attempts = 0;
    bool status_requested = false;
    bool adapter_restarted = false;
    uint32_t gap_event_before = 0;
    bool awaiting_gap_event = false;
    int gap_failure = 0;

    void clear_queue() {
        portENTER_CRITICAL(&queue_lock);
        pending.clear();
        portEXIT_CRITICAL(&queue_lock);
    }
    bool pop(Message &out) {
        bool have = false;
        portENTER_CRITICAL(&queue_lock);
        have = pending.pop(out);
        portEXIT_CRITICAL(&queue_lock);
        return have;
    }
    void enqueue(const uint8_t *data, size_t length, uint32_t expected_epoch) {
        if (!connected.load() || epoch.load() != expected_epoch) return;
        if (!length || length > MAX_NOTIFICATION) { ++dropped; queue_problem = true; return; }
        bool accepted = false;
        portENTER_CRITICAL(&queue_lock);
        // Recheck under the same lock as clear_queue: a late callback that
        // passed the first check must not repopulate a disabled queue afterward.
        if (connected.load() && epoch.load() == expected_epoch)
            accepted = pending.push(data, length, esp_timer_get_time(), expected_epoch);
        portEXIT_CRITICAL(&queue_lock);
        if (accepted) { ++notifications; last_notification_ms = millis(); }
        else { ++dropped; queue_problem = true; }
    }
};

struct Runtime {
    Link links[2];
    battery_bms::JkFeed jk;
    battery_bms::PaceFeed pace;
    SemaphoreHandle_t operation_lock = nullptr;
    TimerHandle_t guard_timer = nullptr;
    std::atomic<bool> initialized{false}, starting{true};
    std::atomic<uint32_t> startup_idle_revision{0};
    Runtime() {
        for (unsigned i = 0; i < 2; ++i) { links[i].owner = this; links[i].index = i; }
    }
    ~Runtime() {
        for (auto &link : links) {
            if (link.queue) { wipe(link.queue, link.capacity * sizeof(Message)); delete[] link.queue; }
            if (link.feed_lock) vSemaphoreDelete(link.feed_lock);
        }
        if (operation_lock) vSemaphoreDelete(operation_lock);
        if (guard_timer) xTimerDelete(guard_timer, 0); // Only destroyed before it has been started.
    }
};

portMUX_TYPE settings_lock = portMUX_INITIALIZER_UNLOCKED;
Settings desired;
Runtime *runtime = nullptr;
bool creation_in_progress = false;
uint32_t desired_revision = 0;
// Maintenance/diagnostic boots retain the saved bindings without starting the
// radio. apply() resumes only after the caller has released its TLS worker.
bool boot_deferred = false;

Settings settings_copy(uint32_t *revision = nullptr) {
    portENTER_CRITICAL(&settings_lock);
    Settings copy = desired;
    if (revision) *revision = desired_revision;
    portEXIT_CRITICAL(&settings_lock);
    return copy;
}

void Callbacks::onConnect(NimBLEClient *) {
    link->connected = true; ++link->connections;
    ++link->gap_events;
}
void Callbacks::onConnectFail(NimBLEClient *, int reason) {
    link->last_error = reason; link->connected = false;
    ++link->gap_events;
}
void Callbacks::onDisconnect(NimBLEClient *, int reason) {
    link->last_error = reason; link->connected = false; ++link->epoch;
    ++link->gap_events;
}

void disconnected(Link &link) {
    link.connected = false; ++link.epoch; link.clear_queue();
    xSemaphoreTake(link.feed_lock, portMAX_DELAY);
    if (!link.index) link.owner->jk.connected(false);
    else link.owner->pace.connected(false);
    xSemaphoreGive(link.feed_lock);
    link.notify = link.write = link.metadata = nullptr;
    link.status_requested = false; link.identity_attempts = 0;
    link.adapter_restarted = false;
    link.queue_problem = false;
}

void close_link(Link &link) {
    link.deadline_ms = 0;
    if (link.client && link.client->isConnected()) link.client->disconnect();
    // The NimBLE client is deliberately not deleted while a disconnect callback
    // could still be pending. Attribute pointers are used only by this worker.
    disconnected(link);
}

void retry(Link &link, int error) {
    close_link(link);
    link.last_error = error;
    link.state = State::BACKOFF;
    link.next_retry_ms = millis() + link.backoff_ms;
    link.backoff_ms = link.backoff_ms < RETRY_MAX_MS / 2 ? link.backoff_ms * 2 : RETRY_MAX_MS;
}

bool read_metadata(Link &link) {
    if (!link.metadata || !link.connected.load()) return false;
    link.deadline_ms = operation_deadline();
    const int64_t begin_us = esp_timer_get_time();
    NimBLEAttValue bytes = link.metadata->readValue();
    const int64_t end_us = esp_timer_get_time();
    link.deadline_ms = 0;
    xSemaphoreTake(link.feed_lock, portMAX_DELAY);
    const bool had_anchor = link.owner->pace.anchored();
    const uint32_t previous_boot = link.owner->pace.boot_id();
    const bool accepted = link.owner->pace.anchor(bytes.data(), bytes.size(), begin_us, end_us);
    if (accepted && had_anchor && previous_boot != link.owner->pace.boot_id()) {
        // A metadata epoch change without a GAP disconnect still invalidates
        // queued notifications. Reconnect rather than mix two source clocks.
        link.adapter_restarted = true;
        link.owner->pace.connected(false);
    }
    xSemaphoreGive(link.feed_lock);
    if (accepted) link.metadata_ms = millis();
    return accepted;
}

bool read_request(Link &link, uint8_t opcode) {
    uint8_t request[20];
    if (!link.write || !battery_bms::JkFeed::request(opcode, request)) return false;
    link.deadline_ms = operation_deadline();
    const bool sent = link.write->writeValue(request, sizeof request, !link.write->canWriteNoResponse());
    link.deadline_ms = 0; wipe(request, sizeof request);
    link.request_ms = millis();
    return sent;
}

bool current_attempt(uint32_t revision) {
    uint32_t actual;
    const Settings settings = settings_copy(&actual);
    return settings.enabled && actual == revision;
}

// Called only while this worker owns operation_lock. The callback counter is
// necessary: GAP can become inactive just before its final host callback runs.
bool close_jk_attempt(Link &link) {
    const uint32_t end = operation_deadline(CLOSE_GUARD_MS);
    uint32_t quiet_at = 0;
    uint32_t last_event = link.gap_events.load();
    bool quiet = false, cancel_sent = false, disconnect_sent = false;
    for (;;) {
        const bool initiating = ble_gap_conn_active();
        const bool actual = link.client->isConnected();
        if (initiating && link.awaiting_gap_event && !cancel_sent) {
            link.client->cancelConnect(); cancel_sent = true;
        }
        if (actual && !disconnect_sent) {
            link.client->disconnect(); disconnect_sent = true;
        }
        if (!actual) disconnect_sent = false; // Also close a late successful connection.
        const uint32_t events = link.gap_events.load();
        if (events != last_event) { quiet = false; last_event = events; }
        const bool callback_done = !link.awaiting_gap_event || events != link.gap_event_before;
        if (!ble_gap_conn_active() && !link.client->isConnected() && callback_done) {
            if (!quiet) { quiet = true; quiet_at = millis(); }
            if (uint32_t(millis() - quiet_at) >= CLOSE_QUIET_MS) {
                link.awaiting_gap_event = false;
                return true;
            }
        } else quiet = false;
        if (int32_t(millis() - end) >= 0) return false;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

bool fail_jk_attempt(Link &link, int reason) {
    link.gap_failure = reason;
    link.state = State::CLOSING;
    const bool closed = close_jk_attempt(link);
    disconnected(link);
    link.last_error = reason;
    if (closed) link.state = State::WAITING;
    return false;
}

bool connect_jk(Link &link, const NimBLEAddress &address) {
    const uint32_t revision = link.configuration_revision.load();
    const uint32_t end = link.deadline_ms.load();
    if (!current_attempt(revision)) { link.last_error = CONFIG_CHANGED_ERROR; return false; }
    link.gap_event_before = link.gap_events.load();
    link.awaiting_gap_event = true;
    // Async GAP avoids the pinned SDK's synchronous m_pTaskData setup window.
    // JK uses20-byte commands/notifications, supported by the default MTU23.
    if (!link.client->connect(address, true, true, false)) {
        link.awaiting_gap_event = false;
        const int reason = link.client->getLastError();
        return fail_jk_attempt(link, reason ? reason : GAP_TIMEOUT_ERROR);
    }
    for (;;) {
        if (!current_attempt(revision)) return fail_jk_attempt(link, CONFIG_CHANGED_ERROR);
        const bool callback = link.gap_events.load() != link.gap_event_before;
        const bool actual = link.client->isConnected();
        if (callback && actual && link.connected.load()) {
            link.awaiting_gap_event = false;
            return true;
        }
        if (callback && link.last_error.load() && !actual)
            return fail_jk_attempt(link, link.last_error.load());
        if (!end || int32_t(millis() - end) >= 0)
            return fail_jk_attempt(link, GAP_TIMEOUT_ERROR);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

bool connect_link(Link &link, const char *address) {
    link.state = State::CONNECTING; link.last_error = 0; ++link.attempts;
    // GAP discovery needs more airtime when another pack and Wi-Fi share the
    // radio. The GATT operations below retain their shorter individual guard.
    link.deadline_ms = operation_deadline(CONNECT_GUARD_MS);
    const NimBLEAddress peer(std::string(address), BLE_ADDR_PUBLIC);
    const bool connected = link.index ? link.client->connect(peer, true, false, true) : connect_jk(link, peer);
    link.deadline_ms = 0;
    if (!connected || !link.connected.load()) return false;
    link.connected_ms = millis();
    link.state = State::DISCOVERING;
    xSemaphoreTake(link.feed_lock, portMAX_DELAY);
    if (!link.index) link.owner->jk.connected(true); else link.owner->pace.connected(true);
    xSemaphoreGive(link.feed_lock);
    link.deadline_ms = operation_deadline();
    NimBLERemoteService *service = !link.index ? link.client->getService(NimBLEUUID(uint16_t(0xFFE0)))
                                              : link.client->getService(PACE_SERVICE);
    link.deadline_ms = 0;
    if (!service) { link.last_error = SERVICE_ERROR; return false; }
    if (!link.index) {
        // Do not assume UUID uniqueness: older JK modules combine properties,
        // newer modules can have two separate FFE1 characteristics.
        link.deadline_ms = operation_deadline();
        unsigned writers = 0, notifiers = 0;
        for (auto *characteristic : service->getCharacteristics(true)) {
            if (characteristic->getUUID() != NimBLEUUID(uint16_t(0xFFE1))) continue;
            if (characteristic->canNotify()) { link.notify = characteristic; ++notifiers; }
            if (characteristic->canWriteNoResponse() || characteristic->canWrite()) {
                link.write = characteristic; ++writers;
            }
        }
        link.deadline_ms = 0;
        if (writers != 1 || notifiers != 1) { link.last_error = PROPERTIES_ERROR; return false; }
    } else {
        link.deadline_ms = operation_deadline();
        link.metadata = service->getCharacteristic(PACE_METADATA);
        link.notify = service->getCharacteristic(PACE_FRAMES);
        link.deadline_ms = 0;
        if (!link.metadata || !link.metadata->canRead() || !link.notify || !link.notify->canNotify()) {
            link.last_error = PROPERTIES_ERROR; return false;
        }
        if (!read_metadata(link)) { link.state = State::CLOCK_WAIT; link.last_error = METADATA_ERROR; }
    }
    const uint32_t epoch = link.epoch.load();
    link.deadline_ms = operation_deadline();
    const bool subscribed = link.notify->subscribe(true,
        [&link, epoch](NimBLERemoteCharacteristic *, uint8_t *data, size_t length, bool) {
            link.enqueue(data, length, epoch);
        }, true);
    link.deadline_ms = 0;
    if (!subscribed) { link.last_error = SUBSCRIBE_ERROR; return false; }
    if (!link.index) {
        link.state = State::IDENTIFYING;
        if (!read_request(link, 0x97)) { link.last_error = READ_REQUEST_ERROR; return false; }
        link.identity_attempts = 1;
    } else if (link.state != State::CLOCK_WAIT) link.state = State::STREAMING;
    link.rssi = link.client->getRssi(); link.backoff_ms = 1000;
    return true;
}

void process_notifications(Link &link) {
    Message message;
    while (link.pop(message)) {
        if (message.epoch == link.epoch.load() && link.connected.load()) {
            xSemaphoreTake(link.feed_lock, portMAX_DELAY);
            if (!link.index) link.owner->jk.accept(message.data, message.length, message.at_us);
            else link.owner->pace.accept(message.data, message.length, message.at_us);
            xSemaphoreGive(link.feed_lock);
        }
        wipe(&message, sizeof message);
    }
}

void run_link(void *argument) {
    Link &link = *static_cast<Link *>(argument);
    link.task = xTaskGetCurrentTaskHandle();
    for (;;) {
        uint32_t revision;
        const Settings settings = settings_copy(&revision);
        const char *wanted = link.index ? settings.pace_address : settings.jk_address;
        const bool configured = settings.enabled && wanted[0];
        if (link.state.load() == State::CLOSING) {
            if (xSemaphoreTake(link.owner->operation_lock, pdMS_TO_TICKS(50))) {
                const int reason = link.gap_failure;
                const bool closed = close_jk_attempt(link);
                xSemaphoreGive(link.owner->operation_lock);
                if (closed) retry(link, reason);
            }
            vTaskDelay(pdMS_TO_TICKS(20)); continue;
        }
        if (!configured || strcmp(wanted, link.active_address)) {
            if (link.active_address[0] || link.connected.load()) close_link(link);
            if (configured) memcpy(link.active_address, wanted, sizeof link.active_address);
            else memset(link.active_address, 0, sizeof link.active_address);
            link.next_retry_ms = 0; link.backoff_ms = 1000;
            link.state = configured ? State::WAITING : State::OFF;
            link.configuration_revision = revision;
            if (!configured) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
        }
        link.configuration_revision = revision;
        if (!link.connected.load()) {
            // Invalidates every sample promptly after a peer drops, without
            // waiting for the next background connection attempt.
            disconnected(link);
            if (link.client->isConnected()) {
                link.client->disconnect();
                vTaskDelay(pdMS_TO_TICKS(100)); continue;
            }
            const uint32_t retry_at = link.next_retry_ms.load();
            if (retry_at && int32_t(millis() - retry_at) < 0) {
                vTaskDelay(pdMS_TO_TICKS(100)); continue;
            }
            if (!xSemaphoreTake(link.owner->operation_lock, pdMS_TO_TICKS(50))) continue;
            // A canceled initiator retains global GAP ownership until its
            // final callback and closure have settled. Do not race it with
            // another peer's connection procedure between cleanup retries.
            if (link.owner->links[0].state.load() == State::CLOSING ||
                link.owner->links[1].state.load() == State::CLOSING) {
                xSemaphoreGive(link.owner->operation_lock);
                vTaskDelay(pdMS_TO_TICKS(20)); continue;
            }
            // Only one GAP connection procedure runs at a time; the other
            // worker continues draining notifications throughout this wait.
            const bool connected = connect_link(link, wanted);
            const int error = link.last_error.load() ? link.last_error.load() : link.client->getLastError();
            xSemaphoreGive(link.owner->operation_lock);
            if (!connected) {
                if (link.state.load() != State::CLOSING) retry(link, error);
                continue;
            }
        }
        // A cancelled connection can complete late. It must pass service and
        // subscription setup before either feed can consume its notifications.
        if (!link.notify || (!link.index && !link.write) || (link.index && !link.metadata)) {
            retry(link, SERVICE_ERROR); continue;
        }
        // A lost JK fragment must never be joined to the next record. Reset the
        // transport and re-identify before accepting another telemetry stream.
        if (!link.index && link.queue_problem.exchange(false)) {
            retry(link, NOTIFICATION_OVERFLOW); continue;
        }
        if (link.adapter_restarted) { retry(link, ADAPTER_RESTARTED); continue; }
        process_notifications(link);
        if (!link.connected.load()) continue;
        if (!link.index) {
            xSemaphoreTake(link.feed_lock, portMAX_DELAY);
            const bool ready = link.owner->jk.profile_ready();
            const auto sample = link.owner->jk.view(esp_timer_get_time());
            xSemaphoreGive(link.feed_lock);
            if (!ready && uint32_t(millis() - link.request_ms) > 3000) {
                if (link.identity_attempts >= 3) { retry(link, IDENTITY_TIMEOUT); continue; }
                if (xSemaphoreTake(link.owner->operation_lock, 0)) {
                    const bool sent = read_request(link, 0x97); ++link.identity_attempts;
                    xSemaphoreGive(link.owner->operation_lock);
                    if (!sent) { retry(link, READ_REQUEST_ERROR); continue; }
                }
            } else if (ready && !link.status_requested) {
                if (xSemaphoreTake(link.owner->operation_lock, 0)) {
                    const bool sent = read_request(link, 0x96);
                    xSemaphoreGive(link.owner->operation_lock);
                    if (!sent) { retry(link, READ_REQUEST_ERROR); continue; }
                    link.status_requested = true; link.state = State::STREAMING;
                }
            } else if (ready && link.status_requested && !sample.has(battery_bms::SOC) &&
                       uint32_t(millis() - link.request_ms) > 15000) {
                retry(link, STREAM_TIMEOUT); continue;
            }
        } else if (link.state == State::CLOCK_WAIT || uint32_t(millis() - link.metadata_ms) > 20000) {
            if (uint32_t(millis() - link.request_ms) > 1000 &&
                xSemaphoreTake(link.owner->operation_lock, 0)) {
                const bool accepted = read_metadata(link); link.request_ms = millis();
                xSemaphoreGive(link.owner->operation_lock);
                link.state = accepted ? State::STREAMING : State::CLOCK_WAIT;
                if (!accepted) link.last_error = METADATA_ERROR;
            }
        }
        // NimBLE's synchronous GATT waits own the default task-notification
        // slot. Application callbacks must not signal or consume that slot:
        // doing so can release a GATT wait before its stack callback is done.
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void guard(Runtime &r) {
    if (!r.initialized.load()) return;
    const bool enabled = settings_copy().enabled;
    const uint32_t now = millis();
    for (auto &link : r.links) {
        uint32_t deadline = link.deadline_ms.load();
        if (!link.client) continue;
        if ((!enabled || (deadline && int32_t(now - deadline) >= 0)) &&
            (!deadline || link.deadline_ms.compare_exchange_strong(deadline, 0))) {
            // Both calls only enqueue/cancel the GAP operation and return.
            // Never perform GATT reads or wait for events on the timer task.
            if (link.state.load() == State::CONNECTING) link.client->cancelConnect();
            if (link.client->isConnected()) link.client->disconnect();
        }
    }
}

void timer_guard(TimerHandle_t timer) {
    guard(*static_cast<Runtime *>(pvTimerGetTimerID(timer)));
}

bool prepare_coexistence() {
    // IDF 4.4 aborts in coex_core_enable when Wi-Fi was set to PS_NONE.
    // Use Arduino's enum overload as well as the driver query: its cached
    // sleep policy is reapplied whenever Wi-Fi recovery recreates the driver.
    // setSleep returns false when the cache already has the requested value.
    WiFi.setSleep(WIFI_PS_MIN_MODEM);
    wifi_ps_type_t actual = WIFI_PS_NONE;
    esp_err_t result = esp_wifi_get_ps(&actual);
    if (result == ESP_ERR_WIFI_NOT_INIT)
        return WiFi.getSleep() == WIFI_PS_MIN_MODEM;
    if (result != ESP_OK) return false;
    if (actual != WIFI_PS_MIN_MODEM) {
        if (esp_wifi_set_ps(WIFI_PS_MIN_MODEM) != ESP_OK) return false;
        result = esp_wifi_get_ps(&actual);
    }
    return result == ESP_OK && actual == WIFI_PS_MIN_MODEM &&
           WiFi.getSleep() == WIFI_PS_MIN_MODEM;
}

void start_worker(void *argument) {
    auto &r = *static_cast<Runtime *>(argument);
    uint32_t retry_at = 0;
    for (;;) {
        uint32_t revision;
        const Settings settings = settings_copy(&revision);
        if (!settings.enabled) {
            r.startup_idle_revision = revision;
            vTaskDelay(pdMS_TO_TICKS(100)); continue;
        }
        r.startup_idle_revision = 0;
        if (retry_at && int32_t(millis() - retry_at) < 0) {
            vTaskDelay(pdMS_TO_TICKS(100)); continue;
        }
        const bool coexistence = prepare_coexistence();
        if (coexistence && NimBLEDevice::init("brain_zero")) break;
        for (auto &link : r.links) {
            link.state = State::STACK_FAILED;
            link.last_error = coexistence ? 0 : WIFI_COEX_ERROR;
        }
        // Keep an initialization failure's backoff responsive to a temporary
        // disable, so a maintenance restart can acknowledge quiescence.
        retry_at = operation_deadline(RETRY_MAX_MS);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    NimBLEDevice::setMTU(64);
    for (auto &link : r.links) {
        link.client = NimBLEDevice::createClient();
        if (!link.client) { link.state = State::NO_MEMORY; continue; }
        link.client->setClientCallbacks(&link.callbacks, false);
        link.client->setSelfDelete(false, false);
        link.client->setConnectTimeout(CONNECT_TIMEOUT_MS);
        // 30-60ms interval, no slave latency, six-second supervision timeout.
        link.client->setConnectionParams(24, 48, 0, 600);
        link.state = State::WAITING;
    }
    while (xTimerStart(r.guard_timer, 0) != pdPASS) {
        for (auto &link : r.links) link.state = State::NO_MEMORY;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    r.initialized = true;
    if (r.links[1].client) {
        TaskHandle_t task = nullptr;
        if (xTaskCreate(run_link, "bms-pace", 4096, &r.links[1], 1, &task) != pdPASS)
            r.links[1].state = State::NO_MEMORY;
        else r.links[1].task = task;
    }
    r.starting = false;
    if (r.links[0].client) run_link(&r.links[0]);
    // Keep the task handle valid for health snapshots even after allocation
    // failure; deleting it would leave a dangling stack-watermark handle.
    for (;;) vTaskDelay(pdMS_TO_TICKS(30000));
}

bool create_runtime() {
    Runtime *created = new (std::nothrow) Runtime;
    if (!created) return false;
    created->operation_lock = xSemaphoreCreateMutex();
    created->guard_timer = xTimerCreate("bms-guard", pdMS_TO_TICKS(250), pdTRUE, created, timer_guard);
    for (auto &link : created->links) {
        link.capacity = link.index ? 8 : 16;
        link.queue = new (std::nothrow) Message[link.capacity]();
        if (link.queue) link.pending.bind(link.queue, link.capacity);
        link.feed_lock = xSemaphoreCreateMutex();
    }
    if (!created->operation_lock || !created->guard_timer || !created->links[0].feed_lock || !created->links[1].feed_lock ||
        !created->links[0].queue || !created->links[1].queue) { delete created; return false; }
    TaskHandle_t task = nullptr;
    if (xTaskCreate(start_worker, "bms-jk", 4096, created, 1, &task) != pdPASS) { delete created; return false; }
    created->links[0].task = task;
    // Publish only after success. Snapshot readers can never retain a pointer
    // that an allocation/task-creation failure then destroys.
    portENTER_CRITICAL(&settings_lock); runtime = created; portEXIT_CRITICAL(&settings_lock);
    return true;
}

void link_view(Link &link, LinkView &out, const Settings &settings, int64_t now_us) {
    out.state = link.state.load();
    out.configured = settings.enabled && (link.index ? settings.pace_address[0] : settings.jk_address[0]);
    out.connected = link.connected.load(); out.attempts = link.attempts.load();
    out.hci_connected = link.owner->initialized.load() && link.client && link.client->isConnected();
    out.connections = link.connections.load(); out.notifications = link.notifications.load();
    out.dropped = link.dropped.load(); out.last_error = link.last_error.load(); out.rssi = link.rssi.load();
    const uint32_t now_ms = uint32_t(now_us / 1000), last = link.last_notification_ms.load();
    if (out.notifications) out.notification_age_s = uint32_t(now_ms - last) / 1000.f;
    const uint32_t next_retry_ms = link.next_retry_ms.load();
    if (out.state == State::BACKOFF && int32_t(next_retry_ms - now_ms) > 0)
        out.retry_ms = next_retry_ms - now_ms;
    if (link.task) out.stack_min_free = uxTaskGetStackHighWaterMark(link.task);
}

}  // namespace

bool begin(const Settings &settings, bool defer) {
#if BRAIN_ZERO_BLE_BOOT_RECOVERY
    defer = true;
#endif
    if (defer && settings.enabled) {
        Settings canonical = settings;
        if (!normalize(canonical)) return false;
        portENTER_CRITICAL(&settings_lock);
        const bool can_defer = runtime == nullptr && !creation_in_progress;
        if (can_defer) {
            desired = canonical;
            ++desired_revision;
            boot_deferred = true;
        }
        portEXIT_CRITICAL(&settings_lock);
        return can_defer;
    }
    return apply(settings);
}

bool allocated() {
    portENTER_CRITICAL(&settings_lock);
    const bool have_resources = runtime != nullptr || creation_in_progress;
    portEXIT_CRITICAL(&settings_lock);
    return have_resources;
}

bool quiescent() {
    portENTER_CRITICAL(&settings_lock);
    Runtime *r = runtime;
    const bool creating = creation_in_progress, enabled = desired.enabled;
    const uint32_t revision = desired_revision;
    portEXIT_CRITICAL(&settings_lock);
    if (creating) return false;
    if (!r) return true;
    if (enabled) return false;
    if (!r->initialized.load()) return r->startup_idle_revision.load() == revision;
    if (r->starting.load()) return false;
    if (ble_gap_conn_active()) return false;
    for (auto &link : r->links) {
        if (!link.client) continue;
        if (link.client->isConnected() || link.deadline_ms.load()) return false;
        if (link.task && (link.state.load() != State::OFF ||
                          link.configuration_revision.load() != revision)) return false;
    }
    return true;
}

bool radio_requires_sleep() {
    portENTER_CRITICAL(&settings_lock);
    const bool required = desired.enabled || runtime != nullptr || creation_in_progress;
    portEXIT_CRITICAL(&settings_lock);
    return required;
}

bool apply(const Settings &settings) {
    Settings canonical = settings;
    if (!normalize(canonical)) return false;
    bool create = false;
    portENTER_CRITICAL(&settings_lock);
    const Settings previous = desired;
    const bool previously_deferred = boot_deferred;
    desired = canonical;
    boot_deferred = false;
    const uint32_t revision = ++desired_revision;
    if (canonical.enabled && !runtime && !creation_in_progress) {
        creation_in_progress = true; create = true;
    }
    portEXIT_CRITICAL(&settings_lock);
    bool success = true;
    if (create) {
        success = create_runtime();
        portENTER_CRITICAL(&settings_lock);
        creation_in_progress = false;
        if (!success && desired_revision == revision) {
            desired = previous;
            boot_deferred = previously_deferred;
        }
        portEXIT_CRITICAL(&settings_lock);
    }
    return success;
}

void loop() {
    portENTER_CRITICAL(&settings_lock); Runtime *r = runtime; portEXIT_CRITICAL(&settings_lock);
    if (r) guard(*r);
}

void view(View &out, int64_t now_us) {
    // Construct the defaults directly in the caller's object. Assigning a
    // View{} here puts a second full snapshot on the small Arduino task stack.
    out.~View();
    new (&out) View{};
    portENTER_CRITICAL(&settings_lock);
    const Settings settings = desired;
    Runtime *r = runtime;
    const uint32_t revision = desired_revision;
    const bool creating = creation_in_progress;
    const bool deferred = boot_deferred;
    portEXIT_CRITICAL(&settings_lock);
    out.enabled = settings.enabled;
    out.deferred = deferred;
    if (r) {
        out.initialized = r->initialized.load(); out.starting = r->starting.load();
        link_view(r->links[0], out.jk_link, settings, now_us);
        link_view(r->links[1], out.pace_link, settings, now_us);
        if (xSemaphoreTake(r->links[0].feed_lock, 0)) {
            out.jk = r->jk.view(now_us); out.jk_link.decoder = r->jk.stats();
            xSemaphoreGive(r->links[0].feed_lock);
        }
        if (xSemaphoreTake(r->links[1].feed_lock, 0)) {
            out.pace = r->pace.view(now_us); out.pace_link.decoder = r->pace.stats();
            xSemaphoreGive(r->links[1].feed_lock);
        }
        if (r->links[0].configuration_revision.load() != revision) out.jk = {};
        if (r->links[1].configuration_revision.load() != revision) out.pace = {};
    } else { out.starting = creating; }
    if (deferred) {
        out.jk_link.configured = settings.enabled && settings.jk_address[0];
        out.pace_link.configured = settings.enabled && settings.pace_address[0];
        if (out.jk_link.configured) out.jk_link.state = State::DEFERRED;
        if (out.pace_link.configured) out.pace_link.state = State::DEFERRED;
    }
    // A disable must invalidate the public snapshot immediately, even if a
    // worker is finishing a blocked BLE operation or disconnect callback.
    if (!settings.enabled || !settings.jk_address[0]) { out.jk = {}; out.jk_link.state = State::OFF; }
    if (!settings.enabled || !settings.pace_address[0]) { out.pace = {}; out.pace_link.state = State::OFF; }
    if (!out.jk_link.connected) { out.jk.valid = 0; out.jk.connected = out.jk.complete = false; }
    if (!out.pace_link.connected) { out.pace.valid = 0; out.pace.connected = out.pace.complete = false; }
    multi_heap_info_t heap{};
    heap_caps_get_info(&heap, MALLOC_CAP_INTERNAL);
    out.free_heap = heap.total_free_bytes; out.min_free_heap = heap.minimum_free_bytes;
    out.largest_free_block = heap.largest_free_block;
    out.internal_heap_total = heap.total_free_bytes + heap.total_allocated_bytes;
    out.internal_heap_used = heap.total_allocated_bytes;
}

}  // namespace battery_ble
#else
namespace battery_ble {
bool allocated() { return false; }
bool quiescent() { return true; }
bool radio_requires_sleep() { return false; }
}
#endif  // ARDUINO_ARCH_ESP32
