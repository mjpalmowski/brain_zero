// When a Home Assistant request on brain_zero/<id>/request is answered. Pure, for the native tests.
//
// A request is never dropped only because the client's outbox is busy: with Bluetooth enabled the
// Wi-Fi radio runs in modem sleep, broker acknowledgements arrive later, and retained QoS 1 battery
// and Victron publishes keep the outbox above its limit for a large share of the time. Such a
// request waits instead, while background publishing pauses, and is answered once the outbox
// drains. Only a request too old for Home Assistant's 5 s wait is dropped.
#pragma once
#include <stdint.h>

namespace mqtt_rpc {

enum class Gate : uint8_t { ANSWER, WAIT, DROP_STALE, IGNORE };

constexpr int64_t MAX_AGE_US = 3000000;  // Home Assistant gives up after 5 s
constexpr uint32_t MIN_GAP_MS = 100;     // between answers, so one client cannot flood the loop

inline Gate gate(bool retained, bool connected, uint32_t since_answer_ms, int64_t age_us, int outbox_bytes,
                 int outbox_limit) {
    if (retained) return Gate::IGNORE;  // a stale retained request must never run
    if (age_us < 0 || age_us > MAX_AGE_US) return Gate::DROP_STALE;
    if (!connected || since_answer_ms < MIN_GAP_MS || outbox_bytes >= outbox_limit) return Gate::WAIT;
    return Gate::ANSWER;
}

}  // namespace mqtt_rpc
