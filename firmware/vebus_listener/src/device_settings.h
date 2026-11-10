#pragma once
#include <stdint.h>
#include <string.h>

namespace device_settings {
struct Network {
    uint32_t version = 1;
    char ssid[33] = {};
    char password[65] = {};
};

inline bool valid(const Network &n) {
    if (n.version != 1 || !n.ssid[0] || !memchr(n.ssid, 0, sizeof n.ssid) ||
        !memchr(n.password, 0, sizeof n.password)) return false;
    const size_t len = strlen(n.password);
    if (len < 8) return false;
    if (len == 64) {
        for (const char *p = n.password; *p; ++p) {
            if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F'))) return false;
        }
    }
    return true;
}

class NetworkTrial {
  public:
    enum class Phase { IDLE, JOIN, VERIFY, REVERT };
    enum class Action { NONE, JOIN, REVERT };
    static constexpr uint32_t WINDOW_MS = 90000;
    Phase phase() const { return phase_; }
    bool pending() const { return phase_ != Phase::IDLE; }
    bool testing() const { return phase_ == Phase::VERIFY; }
    void schedule(uint32_t now) { phase_ = Phase::JOIN; since_ = now; }
    void cancel(uint32_t now) { phase_ = Phase::REVERT; since_ = now; }
    void confirm() { phase_ = Phase::IDLE; }
    uint32_t seconds(uint32_t now) const {
        const uint32_t elapsed = now - since_;
        return testing() && elapsed < WINDOW_MS ? (WINDOW_MS - elapsed + 999) / 1000 : 0;
    }
    Action tick(uint32_t now) {
        if (phase_ == Phase::JOIN && now - since_ >= 750) {
            phase_ = Phase::VERIFY;
            since_ = now;
            return Action::JOIN;
        }
        if ((phase_ == Phase::REVERT && now - since_ >= 750) ||
            (phase_ == Phase::VERIFY && now - since_ >= WINDOW_MS)) {
            phase_ = Phase::IDLE;
            return Action::REVERT;
        }
        return Action::NONE;
    }
  private:
    Phase phase_ = Phase::IDLE;
    uint32_t since_ = 0;
};
}
