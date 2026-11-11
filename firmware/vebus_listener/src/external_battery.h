#pragma once

#include <stddef.h>
#include <stdint.h>

// Optional, runtime-configured MQTT measurements. These are RAM-only samples,
// never inverter settings or charging plans. No broker or battery identity is built in.
namespace external_battery {
constexpr int64_t SOC_FRESH_US = 10000000, CURRENT_FRESH_US = 6000000;
struct View {
    bool enabled = false, connected = false;
    bool soc_valid = false, current_valid = false, voltage_valid = false;
    float soc = 0, voltage = 0, external_current_a = 0;
    float soc_age_s = -1, current_age_s = -1;
    uint32_t accepted = 0, refused = 0;
    // Filled from cached inverter feedback by the controller snapshot function.
    bool sync_verified = false;
    float sync_target = 0, sync_age_s = -1;
    uint32_t sync_sent = 0, sync_current_sent = 0;
};

class Feed {
public:
    void enabled(bool value);
    bool enabled() const { return enabled_; }
    // Once used, refresh zero current even during fallback so the inverter's
    // internal SOC integration is released after the last external SOC sample.
    bool synchronization_started() const { return started_; }
    void connected(bool value);
    void invalidate();
    bool accept(const char *json, size_t length, bool retained, int64_t queued_at,
                int64_t now_us, double utc, const char **why = nullptr);
    View view(int64_t now_us) const;
private:
    bool enabled_ = false, connected_ = false, started_ = false, has_voltage_ = false;
    float soc_ = 0, voltage_ = 0, current_ = 0;
    double soc_stamp_ = 0, current_stamp_ = 0;
    double soc_initial_age_ = 0, current_initial_age_ = 0;
    int64_t soc_at_ = -1, current_at_ = -1;
    uint32_t accepted_ = 0, refused_ = 0;
};
}  // namespace external_battery
