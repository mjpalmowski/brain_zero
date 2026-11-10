// The ESS logic Venus OS runs, for the ESP32 to run instead when no MK3 is on the bus: Venus's ESS
// menu as settings, and from them, the time, the SOC and the Multi's AC-out power, the setpoint and
// the ESS flag word to write. Plain C++ (no Arduino), so test/ess_policy_test.cpp can run it on a PC.
//
// Ported from Venus's dbus-systemcalc-py (delegates/batterylife.py and schedule.py, read on the Venus
// Pi): the minimum-SOC guard ("Optimized without BatteryLife"), BatteryLife with its moving SOC limit,
// keep-batteries-charged, and the five scheduled charge windows with their day codes, windows past
// midnight, 5 % hysteresis and holding at the target. Venus turns their results into bus writes in a
// compiled program (hub4control); what it writes follows our measurements
// (docs/vebus_findings_2026-10-25.md, "ESS settings on the bus"): limits move the setpoint, and hard
// stops (no discharge) set bit 1 of the flag word.
//
// Signs: the settings use Venus's (grid setpoint + = import, as in its menu); the setpoint written on
// the bus is + = export. The Multi holds its AC input at the setpoint, so the battery's power is
//   inverter = -(setpoint + AC-out)          (+ = charging; AC-out + = drawn by the loads)
// which is how charge and discharge limits become bounds on the setpoint.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <time.h>

namespace ess {

constexpr int SCHEDULES = 5;
constexpr int HARD_MAX_W = 5000;   // no setting or setpoint goes beyond this
constexpr int RAMP_W = 1000;       // largest setpoint step per write: the largest measured, settled in 1-1.5 s
constexpr float SLOW_CHARGE_A = 5; // Venus's ForceChargeCurrent: recharging below the SOC limit
constexpr uint16_t FLAG_NO_CHARGE = 1, FLAG_NO_FEED_IN = 2, FLAG_NO_DC_PV_FEED_IN = 4;  // the flag word's bits
constexpr uint16_t MIN_HOLD_CV = 800, MAX_HOLD_CV = 8000;
constexpr int64_t HOLD_SAMPLE_MAX_US = 2000000, HOLD_DELAY_US = 5000000;
constexpr bool valid_hold_voltage(uint16_t cv) { return cv == 0 || (cv >= MIN_HOLD_CV && cv <= MAX_HOLD_CV); }
// A saved absolute threshold from a 48 V installation must not arm a 24 V unit.
constexpr bool hold_voltage_matches_model(uint16_t cv, uint8_t nominal_voltage) {
    return !cv || nominal_voltage != 24 || (cv >= 1900 && cv <= 3300);
}

enum class Mode : uint8_t { OPTIMIZED_BATTERYLIFE = 0, OPTIMIZED = 1, KEEP_CHARGED = 2, EXTERNAL_CONTROL = 3 };

// Venus's ESS states (/Settings/CGwacs/BatteryLife/State), with its numbers.
enum State : uint8_t {
    BL_DISABLED = 0, BL_RESTART = 1, BL_DEFAULT = 2, BL_ABSORPTION = 3, BL_FLOAT = 4, BL_DISCHARGED = 5,
    BL_FORCE_CHARGE = 6, BL_SUSTAIN = 7, BL_LOW_SOC_CHARGE = 8, KEEP_CHARGED = 9, SOC_GUARD_DEFAULT = 10,
    SOC_GUARD_DISCHARGED = 11, SOC_GUARD_LOW_SOC_CHARGE = 12
};

// Days as in Venus: 0-6 Sunday to Saturday, 7 every day, 8 weekdays, 9 weekends, 10 the first of
// each month. Venus disables a slot by making its day negative; here that is `enabled`.
constexpr int DAY_EVERY = 7, DAY_WEEKDAYS = 8, DAY_WEEKENDS = 9, DAY_MONTHLY = 10;

struct Schedule {
    bool enabled = false;
    uint8_t day = DAY_EVERY;
    int32_t start_s = 0;           // local time, seconds after midnight
    int32_t duration_s = 0;        // up to 24 h; a window may run past midnight
    uint8_t soc = 100;             // charge to this SOC, %; 100 charges for the whole window
    bool allow_discharge = false;  // once more than 1 % above the target, discharge as usual
};

struct Settings {
    uint16_t version = 1;
    bool enabled = false;          // run ESS from this ESP32 whenever no other master (MK3) is on the bus
    Mode mode = Mode::OPTIMIZED;
    float min_soc = 10;            // %, "Minimum SOC (unless grid fails)"
    int grid_setpoint_w = 50;      // + = import (Venus's convention); in external control, the setpoint
    int max_charge_w = -1;         // -1 = no limit
    int max_discharge_w = -1;      // inverter power; -1 = no limit, 0 = no discharge at all
    int max_feed_in_w = -1;        // export to the grid; -1 = no limit
    int max_import_w = -1;         // import from the grid (peak shaving); -1 = no limit
    bool always_peak_shave = false;  // keep to the import limit below the minimum SOC too
    bool dc_pv_feed_in = false;    // feed in DC-coupled PV overvoltage (flag bit 2 clear)
    bool ext_no_charge = false;    // external control only: flag bit 0
    bool ext_no_feed_in = false;   // external control only: flag bit 1
    int safety_max_w = 100;        // the setpoint never goes beyond this, either way
    Schedule schedule[SCHEDULES];
    char tz[48] = "GMT0BST,M3.5.0/1,M10.5.0";  // POSIX time zone for the schedules: Europe/London
    uint16_t voltage_hold_cv = 0;  // 0 disables the guard. Stored separately from the original settings blob.
    uint8_t schedule_grid_mask = 0; // one bit per slot; persisted separately from the v1 settings blob
    bool grid_after_target(int slot) const { return (schedule_grid_mask & (1u << slot)) != 0; }
    void grid_after_target(int slot, bool on) {
        if (on) schedule_grid_mask |= 1u << slot;
        else schedule_grid_mask &= ~(1u << slot);
    }
};

// What BatteryLife remembers, kept across restarts as Venus keeps it in its settings.
struct Memory {
    uint16_t version = 1;
    uint8_t state = SOC_GUARD_DEFAULT;
    uint8_t flags = 0;             // reached today: 1 float, 2 absorption, 4 discharged
    float soc_limit = 10;          // BatteryLife's SOC limit, before the minimum SOC is applied
    int64_t discharged_at = 0;     // UTC seconds when the SOC limit was last reached; 0 = not known
};

struct Inputs {
    bool clock_valid = false;
    int64_t utc = 0;               // seconds
    struct tm local = {};          // the same moment as local time (tm_wday, tm_mday, hour, min, sec)
    bool soc_valid = false;
    float soc = 0;                 // %
    bool power_valid = false;
    float acout_w = 0;             // + = drawn at AC-out
    float bat_v = 52;
    bool grid_hold = false;        // overnight target reached, until the local window ends
    bool charge_window = false;    // recurring or dated MultiPlus charging is scheduled, even if already at its target
    int64_t now = 0, voltage_at = -1;  // monotonic microseconds; bat_v is measured when voltage_at is recent
};

struct Decision {
    bool write = false;            // write the setpoint (enabled)
    int setpoint_w = 0;            // on the bus, + = export: limited, capped and ramped
    int target_w = 0;              // the same before the ramp
    uint16_t flags = FLAG_NO_DC_PV_FEED_IN;
    uint8_t state = SOC_GUARD_DEFAULT;
    int8_t schedule = -1;          // the scheduled charge window in force, or -1
    bool charging = false;         // charging on purpose: schedule, keep charged, recharge
    bool grid_hold = false;
    bool voltage_hold = false;
    bool charge_inhibited = false;  // voltage protection allows MultiPlus charging only in a scheduled window
    bool capped = false;           // the safety cap cut the setpoint
    float active_soc_limit = -1;   // no discharge at or below this SOC; -1 = none
    char why[112] = "";            // what the setpoint follows, for the page
};

const char *state_name(uint8_t state);
const char *mode_name(Mode mode);

class Policy {
  public:
    // What to write now. `last_w` is the setpoint written last (or the Multi's present one), for the
    // ramp. Steps the states in `memory`; save it when changed() says so.
    Decision decide(const Settings &s, const Inputs &in, int last_w);
    Memory memory;
    bool changed() {
        bool c = changed_;
        changed_ = false;
        return c;
    }
    // A schedule's target SOC changed: decide without the 5 % hysteresis once, as Venus does.
    void schedule_changed() { hysteresis_ = false; }
    void reset_schedule_hold() { grid_hold_window_ = -1; schedule_grid_hold_ = false; }
    void restore_voltage_hold(uint16_t cv, bool latched);
    bool voltage_latched() const { return voltage_latched_; }

    float active_soc_limit(const Settings &s) const;

  private:
    void update_voltage_hold(const Settings &s, const Inputs &in, bool charge_window);
    void step_states(const Settings &s, const Inputs &in);
    uint8_t next_state(const Settings &s, const Inputs &in, uint8_t st);
    void on_discharged(const Settings &s, const Inputs &in);
    void on_absorption(const Settings &s);
    void on_float(const Settings &s);
    void adjust_soc_limit(const Settings &s, float delta);
    void set_flags(uint8_t f);
    void set_state(uint8_t st);
    void set_discharged_at(const Inputs &in);
    bool changed_ = false;
    bool hysteresis_ = true;
    bool force_charge_ = false;    // the schedule's, kept while within 5 % below the target
    bool schedule_grid_hold_ = false;
    int grid_hold_window_ = -1, grid_hold_day_ = -1;
    int reset_day_ = -1;           // the day BatteryLife's flags were last cleared (tm_yday)
    uint16_t voltage_threshold_cv_ = 0;
    bool voltage_latched_ = false;
    int64_t low_since_ = -1, last_low_sample_ = -1;
};

// The window a schedule is in at local time `t` (wall clock, like Venus's naive datetimes): -1, or
// the first schedule whose window contains it, started today or yesterday.
int active_window(const Schedule *schedules, int n, const struct tm &t, int *start_day = nullptr);

// Range checks for settings from the page: false, with the reason, if any value is out of range.
bool validate(const Settings &s, char *why, size_t cap);

}  // namespace ess
