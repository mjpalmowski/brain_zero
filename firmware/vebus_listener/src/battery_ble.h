#pragma once

#include <stddef.h>
#include <stdint.h>
#include "battery_bms.h"

// Read-only, explicitly bound BLE clients. No battery settings, MOSFET or CAN
// transmit API is exposed. Addresses stay in the caller's private settings.
namespace battery_ble {

struct Settings {
    uint16_t version = 1;
    bool enabled = false;
    char jk_address[18] = {}, pace_address[18] = {};
    float jk_capacity_ah = 0, pace_capacity_ah = 0;
};

enum class State : uint8_t {
    OFF, STARTING, WAITING, CONNECTING, DISCOVERING, IDENTIFYING,
    CLOCK_WAIT, STREAMING, BACKOFF, NO_MEMORY, STACK_FAILED
};

struct LinkView {
    State state = State::OFF;
    bool configured = false, connected = false;
    uint32_t attempts = 0, connections = 0, notifications = 0, dropped = 0;
    uint32_t retry_ms = 0, stack_min_free = 0;
    int last_error = 0, rssi = 0;
    float notification_age_s = -1;
    battery_bms::Stats decoder;
};

struct View {
    bool enabled = false, initialized = false, starting = false;
    battery_bms::Snapshot jk, pace;
    LinkView jk_link, pace_link;
    uint32_t free_heap = 0, min_free_heap = 0, largest_free_block = 0;
    uint32_t internal_heap_total = 0, internal_heap_used = 0;
};

// Canonicalises public device addresses and validates the complete settings
// before the caller persists them. Empty addresses leave that client disabled.
bool normalize(Settings &settings, const char **why = nullptr);
const char *state_name(State state);

// begin/apply never initialize the radio on the caller's task. Initializing an
// enabled configuration starts low-priority workers; disabled boot is allocation
// free. Disabling disconnects immediately; allocated BLE resources are retained
// until restart to avoid tearing down a host while callbacks are active.
bool begin(const Settings &settings);
bool apply(const Settings &settings);
// IDF 4.4 Wi-Fi/Bluetooth coexistence requires modem sleep. True from an
// enable request through the retained BLE host lifetime, including disconnects.
// Cheap radio-policy query: no snapshots, allocations or blocking BLE calls.
bool radio_requires_sleep();
// Call frequently from the Arduino loop. Bounded, nonblocking cancellation of
// overdue BLE operations; it never takes the inverter's lock or writes VE.Bus.
void loop();
// A busy snapshot returns invalid pack fields rather than blocking control.
void view(View &out, int64_t now_us);

}  // namespace battery_ble
