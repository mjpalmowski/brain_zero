#pragma once
#include <stdint.h>

namespace wifi_recovery {
constexpr uint32_t RETRY_MS = 45000, MAX_RETRY_MS = 180000;
constexpr uint32_t PROBE_MS = 10000, STABLE_MS = 60000;
constexpr uint32_t DISCONNECT_MS = 300, RADIO_OFF_MS = 5000;
constexpr uint32_t RESTART_WAIT_MS = DISCONNECT_MS + RADIO_OFF_MS + 1000;
constexpr unsigned MISSES = 4;

class Policy {
    uint32_t last_ip_ = 0, last_cycle_ = 0, retry_ms_ = RETRY_MS, gateway_ = 0;
    uint32_t healthy_since_ = 0;
    bool confirmed_ = false, cooling_down_ = false, tracking_health_ = false, stable_ = false;
    unsigned misses_ = 0, attempts_ = 0;

public:
    void observe_gateway(uint32_t gateway) {
        if (!gateway) {
            misses_ = 0;
            return;
        }
        if (gateway == gateway_) return;
        gateway_ = gateway;
        confirmed_ = false;
        misses_ = 0;
        tracking_health_ = stable_ = false;
    }

    void probe(bool replied) {
        if (replied) {
            confirmed_ = true;
            misses_ = 0;
        } else if (confirmed_ && misses_ < MISSES) {
            ++misses_;
        }
    }

    // A gateway that has never answered may simply block ICMP.
    bool due(uint32_t now, bool has_ip) {
        if (has_ip) last_ip_ = now;
        if (!has_ip || misses_) {
            tracking_health_ = stable_ = false;
        } else if (!tracking_health_) {
            tracking_health_ = true;
            healthy_since_ = now;
        } else if (now - healthy_since_ >= STABLE_MS) {
            stable_ = true;
            attempts_ = 0;
            retry_ms_ = RETRY_MS;
        }
        if (cooling_down_ && now - last_cycle_ < retry_ms_) return false;
        cooling_down_ = false;
        return has_ip ? misses_ >= MISSES : now - last_ip_ >= RETRY_MS;
    }

    void cycled(uint32_t now) {
        last_cycle_ = last_ip_ = now;
        cooling_down_ = true;
        tracking_health_ = stable_ = false;
        misses_ = 0;
        if (attempts_ < 2) ++attempts_;
        if (retry_ms_ < MAX_RETRY_MS) retry_ms_ *= 2;
    }

    bool confirmed() const { return confirmed_; }
    uint32_t confirmed_gateway() const { return confirmed_ ? gateway_ : 0; }
    bool stable() const { return stable_; }
    bool should_restart_controller() const { return attempts_ >= 2; }
    unsigned misses() const { return misses_; }
};
}
