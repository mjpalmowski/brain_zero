#pragma once
#include <stddef.h>
#include <stdint.h>
#include "battery_bms.h"

namespace battery_control {
enum class SocSource : uint8_t { EXISTING=0, JK=1, PACE=2, BANK=3 };
struct Settings {
    uint16_t version = 1;
    SocSource soc_source = SocSource::EXISTING;
    bool charge_guard = false;
    uint8_t max_soc = 0;             // Explicitly configure all three thresholds before enabling.
    uint16_t max_cell_mv = 0, max_delta_mv = 0;
    uint16_t limited_charge_w = 0;   // 0 pauses requested inverter charging when a threshold trips.
};
struct Inputs {
    int64_t soc_at[3] = {-1,-1,-1};
    float soc[3] = {};
    int64_t voltage_at[3] = {-1,-1,-1};
    float voltage[3] = {};
    int64_t guard_at = -1;
    float highest_soc = 0, min_cell_v = 0, max_cell_v = 0, delta_cell_v = 0;
    bool charge_allowed = false, protections_clear = false;
};
struct View {
    bool enabled = false, limited = false, data_valid = false;
    int charge_cap_w = -1;
    const char *reason = "Off";
};
bool valid(const Settings &settings, char *why=nullptr, size_t cap=0);
Inputs inputs(const battery_bms::Snapshot &jk,const battery_bms::Snapshot &pace,
              const battery_bms::Snapshot &bank,int64_t now_us);
bool selected_soc(const Settings &,const Inputs &,int64_t now_us,float &soc);
bool selected_voltage(const Settings &,const Inputs &,int64_t now_us,float &voltage);
class Guard {
public:
    View update(const Settings &,const Inputs &,int64_t now_us);
    void reset() { latched_=0; }
private:
    uint8_t latched_=0;
};
}
