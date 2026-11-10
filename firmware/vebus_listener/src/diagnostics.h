// Cached diagnostics only. No transport, settings access or control methods.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "charger_settings.h"
#include "fault_report.h"

namespace diagnostics {
constexpr const char *BUILD = "compatibility-profile-20261110.1";
struct Cache {
    int64_t now = 0, policy_at = -1;
    charger::View charger;
    faults::Report faults;
    int led_code = -1;
    bool pace_valid = false, own_soc_valid = false;
    float pace_soc = 0, own_soc = 0;
    uint32_t pace_age_ms = UINT32_MAX;
    bool sync_verified = false;
    float sync_target = 0, sync_age_s = -1;
    uint32_t sync_sent = 0;
    int max_charge_w = -1, max_discharge_w = -1, max_import_w = -1, max_feed_in_w = -1;
    int safety_max_w = 0;
    float minimum_soc = 0;
    bool free_active = false, recharge_active = false, recharge_charge = false, recharge_target_met = false;
    int free_charge_w = 0, recharge_charge_w = 0;
    uint32_t frame_errors = 0, parity_errors = 0, fifo_overflows = 0, buffer_full = 0, oversize = 0;
};
size_t to_json(const Cache &v, char *out, size_t cap);
const char *reset_name(int reason);
}
