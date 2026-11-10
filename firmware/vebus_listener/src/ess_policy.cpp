#include "ess_policy.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace ess {

namespace {

// batterylife.py's constants
constexpr float SOC_SWITCH_OFFSET = 3, SOC_SWITCH_INCREMENT = 5, LOW_SOC_CHARGE_OFFSET = 5;
constexpr float ABSORPTION_LEVEL = 85, FLOAT_LEVEL = 95, SOC_SWITCH_MAX = ABSORPTION_LEVEL - SOC_SWITCH_INCREMENT;
constexpr int64_t FORCE_CHARGE_INTERVAL_S = 24 * 3600;
constexpr uint8_t REACHED_FLOAT = 1, REACHED_ABSORPTION = 2, REACHED_DISCHARGED = 4;
// schedule.py's: within this many % below the target, charging stays as it was
constexpr float SCHEDULE_HYSTERESIS = 5;

float bound(float lo, float v, float hi) { return v < lo ? lo : v > hi ? hi : v; }
int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
int max_of(int a, int b) { return a > b ? a : b; }

bool is_schedule_day(int day, int wday, int mday) {
    if (day < DAY_EVERY) return wday == day;
    switch (day) {
    case DAY_EVERY: return true;
    case DAY_WEEKDAYS: return wday >= 1 && wday <= 5;
    case DAY_WEEKENDS: return wday == 0 || wday == 6;
    default: return mday == 1;  // monthly
    }
}

// "import 50 W", "export 10 W": a grid power in Venus's sign (+ = import) in words.
void grid_words(int w, char *out, size_t cap) {
    if (w == 0) {
        snprintf(out, cap, "0 W");
    } else {
        snprintf(out, cap, "%s %d W", w > 0 ? "import" : "export", w > 0 ? w : -w);
    }
}

void append(char *out, size_t cap, const char *fmt, ...) {
    size_t n = strlen(out);
    if (n + 1 >= cap) return;
    va_list a;
    va_start(a, fmt);
    vsnprintf(out + n, cap - n, fmt, a);
    va_end(a);
}

}  // namespace

const char *state_name(uint8_t state) {
    switch (state) {
    case BL_DISABLED: return "BatteryLife disabled";
    case BL_RESTART: return "restarting";
    case BL_DEFAULT: return "self-consumption";
    case BL_ABSORPTION: return "self-consumption, absorption level";
    case BL_FLOAT: return "self-consumption, float level";
    case BL_DISCHARGED: return "discharge disabled: SOC at the BatteryLife limit";
    case BL_FORCE_CHARGE: return "slow charge: a day at the SOC limit";
    case BL_SUSTAIN: return "sustain";
    case BL_LOW_SOC_CHARGE: return "recharge: 5 % below the minimum SOC";
    case KEEP_CHARGED: return "keep batteries charged";
    case SOC_GUARD_DEFAULT: return "self-consumption";
    case SOC_GUARD_DISCHARGED: return "discharge disabled: minimum SOC reached";
    case SOC_GUARD_LOW_SOC_CHARGE: return "recharge: 5 % below the minimum SOC";
    default: return "unknown";
    }
}

const char *mode_name(Mode mode) {
    switch (mode) {
    case Mode::OPTIMIZED_BATTERYLIFE: return "optimized with BatteryLife";
    case Mode::OPTIMIZED: return "optimized without BatteryLife";
    case Mode::KEEP_CHARGED: return "keep batteries charged";
    case Mode::EXTERNAL_CONTROL: return "external control";
    default: return "unknown";
    }
}

int active_window(const Schedule *schedules, int n, const struct tm &t, int *start_day) {
    int now_s = t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec;
    for (int i = 0; i < n; i++) {
        const Schedule &w = schedules[i];
        if (!w.enabled || w.duration_s <= 0) continue;
        for (int back = 0; back <= 1; back++) {  // started today, or yesterday and still running
            int wday = (t.tm_wday + 7 - back) % 7;
            int mday = t.tm_mday - back;  // yesterday was the 1st exactly when today is the 2nd
            if (!is_schedule_day(w.day, wday, mday)) continue;
            int32_t since = back * 86400 + now_s - w.start_s;
            if (since >= 0 && since < w.duration_s) {
                if (start_day) {
                    int year = t.tm_year + 1900, day = t.tm_yday - back;
                    if (day < 0) {
                        --year;
                        day = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) ? 365 : 364;
                    }
                    *start_day = year * 366 + day;
                }
                return i;
            }
        }
    }
    return -1;
}

// ---------------------------------------------------------------- BatteryLife

void Policy::set_state(uint8_t st) {
    if (memory.state == st) return;
    memory.state = st;
    changed_ = true;
}

void Policy::set_flags(uint8_t f) {
    if (memory.flags == f) return;
    memory.flags = f;
    changed_ = true;
}

void Policy::set_discharged_at(const Inputs &in) {
    int64_t t = in.clock_valid ? in.utc : 0;  // without a clock, the daily check sets it later
    if (memory.discharged_at == t) return;
    memory.discharged_at = t;
    changed_ = true;
}

float Policy::active_soc_limit(const Settings &s) const {
    if (s.min_soc > SOC_SWITCH_MAX) return s.min_soc;
    return bound(0, memory.soc_limit > s.min_soc ? memory.soc_limit : s.min_soc, SOC_SWITCH_MAX);
}

void Policy::adjust_soc_limit(const Settings &s, float delta) {
    float limit = bound(0, (memory.soc_limit > s.min_soc ? memory.soc_limit : s.min_soc) + delta, SOC_SWITCH_MAX);
    if (limit == memory.soc_limit) return;
    memory.soc_limit = limit;
    changed_ = true;
}

void Policy::on_discharged(const Settings &s, const Inputs &in) {
    if (!(memory.flags & REACHED_DISCHARGED)) {
        set_flags(memory.flags | REACHED_DISCHARGED);
        adjust_soc_limit(s, SOC_SWITCH_INCREMENT);
    }
    set_discharged_at(in);
}

void Policy::on_absorption(const Settings &s) {
    if (memory.flags & REACHED_ABSORPTION) return;
    set_flags(memory.flags | REACHED_ABSORPTION);
    adjust_soc_limit(s, -SOC_SWITCH_INCREMENT);
}

void Policy::on_float(const Settings &s) {
    float offset = 0;
    uint8_t f = memory.flags;
    if (!(f & REACHED_ABSORPTION)) {
        offset -= SOC_SWITCH_INCREMENT;
        f |= REACHED_ABSORPTION;
    }
    if (!(f & REACHED_FLOAT)) {
        offset -= SOC_SWITCH_INCREMENT;
        f |= REACHED_FLOAT;
    }
    set_flags(f);
    adjust_soc_limit(s, offset);
}

// One step of Venus's state machine (batterylife.py's _map); returns `st` to stay.
uint8_t Policy::next_state(const Settings &s, const Inputs &in, uint8_t st) {
    const float soc = in.soc, minsoc = s.min_soc;
    const bool sustain = false;  // the Multi's own sustain flag (/Hub4/Sustain): not read from the bus
    const float limit = active_soc_limit(s);
    const bool low = sustain || (limit > 0 && soc <= limit && soc < 100);
    switch (st) {
    case BL_DISABLED: return BL_RESTART;  // the ESS assistant is there
    case BL_RESTART:
        if (low) return sustain ? BL_SUSTAIN : BL_DISCHARGED;
        if (soc >= FLOAT_LEVEL) return BL_FLOAT;
        if (soc >= ABSORPTION_LEVEL) return BL_ABSORPTION;
        return BL_DEFAULT;
    case BL_DEFAULT:
    case BL_ABSORPTION:
    case BL_FLOAT:
        if (low) {
            on_discharged(s, in);
            return sustain ? BL_SUSTAIN : BL_DISCHARGED;
        }
        if (st == BL_DEFAULT) {
            if (soc >= FLOAT_LEVEL) {
                on_float(s);
                return BL_FLOAT;
            }
            if (soc >= ABSORPTION_LEVEL) {
                on_absorption(s);
                return BL_ABSORPTION;
            }
        } else if (st == BL_ABSORPTION) {
            if (soc > FLOAT_LEVEL) {
                on_float(s);
                return BL_FLOAT;
            }
            if (soc < ABSORPTION_LEVEL - SOC_SWITCH_OFFSET) return BL_DEFAULT;
        } else if (soc < FLOAT_LEVEL - SOC_SWITCH_OFFSET) {
            return BL_ABSORPTION;
        }
        return st;
    case BL_DISCHARGED:
    case BL_SUSTAIN:
        if (!sustain && (soc > limit + SOC_SWITCH_OFFSET || soc >= 100)) return BL_DEFAULT;
        if (soc <= minsoc - LOW_SOC_CHARGE_OFFSET) return BL_LOW_SOC_CHARGE;
        return st;
    case BL_LOW_SOC_CHARGE: {
        // Back to the minimum SOC, or to 3 % if that is lower (a minimum of 0 % recharges from 0 %).
        float back = minsoc > SOC_SWITCH_OFFSET ? minsoc : SOC_SWITCH_OFFSET;
        if (soc < (back < 100 ? back : 100)) return st;
        if (memory.discharged_at != 0 && in.clock_valid && in.utc - memory.discharged_at > FORCE_CHARGE_INTERVAL_S) {
            return BL_FORCE_CHARGE;
        }
        return BL_DISCHARGED;
    }
    case BL_FORCE_CHARGE:
        if (!sustain && (soc > limit || soc >= 100)) {
            set_discharged_at(in);
            return BL_DISCHARGED;
        }
        if (soc <= minsoc - LOW_SOC_CHARGE_OFFSET) return BL_LOW_SOC_CHARGE;
        return st;
    case KEEP_CHARGED: return st;
    case SOC_GUARD_DEFAULT:
        if (soc < 100 && minsoc > 0 && soc <= minsoc) return SOC_GUARD_DISCHARGED;
        return st;
    case SOC_GUARD_DISCHARGED:
        if (soc >= (minsoc + SOC_SWITCH_OFFSET < 100 ? minsoc + SOC_SWITCH_OFFSET : 100)) return SOC_GUARD_DEFAULT;
        if (soc <= minsoc - LOW_SOC_CHARGE_OFFSET) return SOC_GUARD_LOW_SOC_CHARGE;
        return st;
    case SOC_GUARD_LOW_SOC_CHARGE:
        if (soc >= (minsoc < 100 ? minsoc : 100)) return SOC_GUARD_DISCHARGED;
        return st;
    default: return BL_DEFAULT;
    }
}

void Policy::step_states(const Settings &s, const Inputs &in) {
    // The mode picks the state machine, as Venus's menu does by writing the state.
    uint8_t st = memory.state;
    switch (s.mode) {
    case Mode::KEEP_CHARGED: st = KEEP_CHARGED; break;
    case Mode::OPTIMIZED:
        if (st < SOC_GUARD_DEFAULT || st > SOC_GUARD_LOW_SOC_CHARGE) st = SOC_GUARD_DEFAULT;
        break;
    case Mode::OPTIMIZED_BATTERYLIFE:
        if (st == BL_DISABLED || st >= KEEP_CHARGED) st = BL_RESTART;
        break;
    default: return;  // external control: Venus's states stand still
    }
    // Venus's 15-minute timer: in the day's first quarter hour BatteryLife forgets the levels it
    // reached, and a day at the SOC limit raises it and starts a slow charge.
    if (in.clock_valid && in.local.tm_hour == 0 && in.local.tm_min < 15 && reset_day_ != in.local.tm_yday) {
        reset_day_ = in.local.tm_yday;
        set_flags(0);
    }
    if ((st == BL_DISCHARGED || st == BL_SUSTAIN) && in.clock_valid) {
        if (memory.discharged_at == 0) {
            set_discharged_at(in);
        } else if (in.utc - memory.discharged_at > FORCE_CHARGE_INTERVAL_S) {
            adjust_soc_limit(s, SOC_SWITCH_INCREMENT);
            st = BL_FORCE_CHARGE;
        }
    }
    // Up to five steps at once, as Venus: a restart at a very low SOC goes straight to recharging.
    if (in.soc_valid) {
        for (int i = 0; i < 5; i++) {
            uint8_t next = next_state(s, in, st);
            if (next == st) break;
            st = next;
        }
    }
    set_state(st);
}

// ---------------------------------------------------------------- the setpoint

void Policy::restore_voltage_hold(uint16_t cv, bool latched) {
    voltage_threshold_cv_ = cv;
    voltage_latched_ = cv != 0 && latched;
    low_since_ = last_low_sample_ = -1;
}

void Policy::update_voltage_hold(const Settings &s, const Inputs &in, bool charge_window) {
    if (voltage_threshold_cv_ != s.voltage_hold_cv) restore_voltage_hold(s.voltage_hold_cv, false);
    if (s.enabled && charge_window) voltage_latched_ = false;
    if (voltage_latched_) return;
    const bool low = s.enabled && voltage_threshold_cv_ && !charge_window &&
                     in.voltage_at >= 0 && in.now >= in.voltage_at && in.now - in.voltage_at < HOLD_SAMPLE_MAX_US &&
                     in.bat_v > 0 && in.bat_v <= voltage_threshold_cv_ / 100.0f;
    if (!low) {
        low_since_ = last_low_sample_ = -1;
        return;
    }
    if (last_low_sample_ < 0 || in.voltage_at < last_low_sample_ ||
        in.voltage_at - last_low_sample_ >= HOLD_SAMPLE_MAX_US) low_since_ = in.voltage_at;
    last_low_sample_ = in.voltage_at;
    voltage_latched_ = in.voltage_at - low_since_ >= HOLD_DELAY_US;
}

Decision Policy::decide(const Settings &s, const Inputs &in, int last_w) {
    Decision d;
    step_states(s, in);
    d.state = memory.state;
    d.write = s.enabled;
    const bool local_schedule = in.clock_valid && s.mode != Mode::KEEP_CHARGED && s.mode != Mode::EXTERNAL_CONTROL;
    int start_day = -1;
    const int window = local_schedule ? active_window(s.schedule, SCHEDULES, in.local, &start_day) : -1;
    if (grid_hold_window_ != window || grid_hold_day_ != start_day) {
        reset_schedule_hold();
        grid_hold_window_ = window;
        grid_hold_day_ = start_day;
    }
    if (!s.enabled || window < 0 || !s.grid_after_target(window)) schedule_grid_hold_ = false;
    else if (in.soc_valid && in.soc >= s.schedule[window].soc) schedule_grid_hold_ = true;
    d.grid_hold = s.enabled && (in.grid_hold || schedule_grid_hold_);
    const bool charge_window = in.charge_window || (window >= 0 && s.max_charge_w != 0);
    update_voltage_hold(s, in, charge_window);
    d.charge_inhibited = s.enabled && s.voltage_hold_cv && !charge_window;
    d.voltage_hold = d.charge_inhibited && voltage_latched_;
    const bool hold = d.grid_hold || d.voltage_hold;
    uint16_t flags = s.dc_pv_feed_in ? 0 : FLAG_NO_DC_PV_FEED_IN;
    if (d.charge_inhibited || schedule_grid_hold_) flags |= FLAG_NO_CHARGE;
    const int out = in.power_valid ? (int)lroundf(in.acout_w) : 0;  // AC-out, + = drawn by the loads
    char grid[24];
    grid_words(s.grid_setpoint_w, grid, sizeof grid);
    int sp = -s.grid_setpoint_w;  // on the bus, + = export
    const char *bound_by = nullptr;  // the last limit that moved the setpoint

    if (s.mode == Mode::EXTERNAL_CONTROL && !hold) {
        if (s.ext_no_charge) flags |= FLAG_NO_CHARGE;
        if (s.ext_no_feed_in) flags |= FLAG_NO_FEED_IN;
        snprintf(d.why, sizeof d.why, "external control: %s", grid);
        force_charge_ = false;
    } else {
        bool no_discharge = false;
        int charge_w = -1;                     // charge at this power from the grid, if >= 0
        int discharge_w = s.max_discharge_w;   // -1 = no limit
        const int max_charge = s.max_charge_w >= 0 ? s.max_charge_w : HARD_MAX_W;
        snprintf(d.why, sizeof d.why, "grid setpoint, %s", grid);
        if (memory.state < KEEP_CHARGED) {
            d.active_soc_limit = active_soc_limit(s);
        } else if (memory.state > KEEP_CHARGED) {
            d.active_soc_limit = s.min_soc;
        }
        if (!in.soc_valid) {
            snprintf(d.why, sizeof d.why, "no SOC yet: grid setpoint, %s", grid);
        } else {
            switch (memory.state) {
            case KEEP_CHARGED:
                charge_w = max_charge;
                no_discharge = true;
                snprintf(d.why, sizeof d.why, "keep batteries charged");
                break;
            case BL_DISCHARGED:
            case BL_SUSTAIN:
            case SOC_GUARD_DISCHARGED:
                no_discharge = true;
                snprintf(d.why, sizeof d.why, "SOC %.1f %% at the %.0f %% limit: no discharge", in.soc, d.active_soc_limit);
                break;
            case BL_FORCE_CHARGE:
            case BL_LOW_SOC_CHARGE:
            case SOC_GUARD_LOW_SOC_CHARGE:
                charge_w = (int)lroundf(SLOW_CHARGE_A * in.bat_v);
                no_discharge = true;
                snprintf(d.why, sizeof d.why, "%s at %d W", state_name(memory.state), charge_w);
                break;
            default: break;
            }
        }

        // Scheduled charging: not while keeping the batteries charged, nor without a clock or an SOC.
        const int w = !hold && in.soc_valid ? window : -1;
        if (w >= 0) {
            const Schedule &sc = s.schedule[w];
            auto reached = [&](float soc) { return sc.soc < 100 && soc >= sc.soc; };  // 100 % never counts
            if (reached(in.soc)) {
                force_charge_ = false;
            } else if (s.grid_after_target(w) || !(hysteresis_ && reached(in.soc + SCHEDULE_HYSTERESIS))) {
                force_charge_ = true;  // within 5 % below the target it stays as it was
            }
            if (force_charge_) {
                charge_w = max_of(charge_w, max_charge);
                snprintf(d.why, sizeof d.why, "scheduled charge %d: to %u %%", w + 1, sc.soc);
            } else if (!(in.soc - sc.soc > 1 && sc.allow_discharge)) {
                // Hold the target: Venus lets through 80-95 % of the DC-coupled PV power, which this
                // ESP32 cannot see, and at least 1 W (0 would set the no-discharge bit).
                if (discharge_w < 0 || discharge_w > 1) discharge_w = 1;
                snprintf(d.why, sizeof d.why, "scheduled charge %d: holding %u %%", w + 1, sc.soc);
            }
            d.schedule = (int8_t)w;
        } else {
            force_charge_ = false;
        }
        hysteresis_ = true;

        if (hold) {
            charge_w = 0;
            discharge_w = 0;
            if (schedule_grid_hold_) {
                d.schedule = int8_t(window);
                snprintf(d.why, sizeof d.why, "scheduled charge %d: grid supply until window ends", window + 1);
            } else {
                snprintf(d.why, sizeof d.why, "%s", d.voltage_hold ? "battery voltage hold: grid until next charge window"
                                                                  : "grid supply until schedule ends");
            }
        } else if (d.charge_inhibited && charge_w >= 0) {
            charge_w = -1;
            snprintf(d.why, sizeof d.why, "battery charging paused until next charge window");
        }

        // The limits that raise the setpoint first, then those that lower it, which win: the feed-in
        // limit over the charge limit, the inverter limit over the import limit.
        if (charge_w >= 0) {
            sp = -charge_w - out;
            d.charging = !hold;
        }
        if (s.max_charge_w >= 0 && -s.max_charge_w - out > sp) {
            sp = -s.max_charge_w - out;
            bound_by = "the charge limit";
        }
        if (s.max_import_w >= 0 && -s.max_import_w > sp) {
            sp = -s.max_import_w;
            bound_by = "the import limit";
        }
        if (discharge_w > 0 && discharge_w - out < sp) {
            sp = discharge_w - out;
            if (discharge_w == s.max_discharge_w) bound_by = "the inverter limit";
        }
        if (s.max_feed_in_w >= 0 && s.max_feed_in_w < sp) {
            sp = s.max_feed_in_w;
            bound_by = "the feed-in limit";
        }
        // Hard stops go in the flag word, as Venus does it: an inverter limit of 0 W, and the SOC
        // states that allow no discharge. Below the SOC limit only "always" peak shaving may still
        // discharge, and only what the import limit needs.
        if (discharge_w == 0) {
            flags |= FLAG_NO_FEED_IN;
            if (s.max_discharge_w == 0) bound_by = "the inverter limit of 0 W";
        } else if (no_discharge) {
            if (s.always_peak_shave && s.max_import_w >= 0 && out > s.max_import_w) {
                sp = -s.max_import_w;
                if (discharge_w > 0 && discharge_w - out < sp) sp = discharge_w - out;
                bound_by = "peak shaving below the SOC limit";
            } else {
                flags |= FLAG_NO_FEED_IN;
            }
        }
        if (!in.power_valid && (charge_w >= 0 || s.max_charge_w >= 0 || discharge_w > 0)) {
            append(d.why, sizeof d.why, ", no AC-out reading");
        }
    }
    if (bound_by) append(d.why, sizeof d.why, ", held by %s", bound_by);

    // The safety cap. Cutting an import would let the battery cover more of the loads than intended,
    // so then the battery is not discharged at all.
    const int cap = clamp(s.safety_max_w, 0, HARD_MAX_W);
    d.target_w = clamp(sp, -cap, cap);
    if (d.target_w != sp) {
        d.capped = true;
        if (sp < -cap) flags |= FLAG_NO_FEED_IN;
        append(d.why, sizeof d.why, ", capped at %d W", cap);
    }
    d.setpoint_w = clamp(last_w + clamp(d.target_w - last_w, -RAMP_W, RAMP_W), -cap, cap);
    d.flags = flags;
    return d;
}

// ---------------------------------------------------------------- settings

bool validate(const Settings &s, char *why, size_t cap) {
    auto limit_ok = [](int w) { return w == -1 || (w >= 0 && w <= HARD_MAX_W); };
    const char *bad = nullptr;
    if ((int)s.mode > (int)Mode::EXTERNAL_CONTROL) {
        bad = "mode must be 0-3";
    } else if (!(s.min_soc >= 0 && s.min_soc <= 100)) {
        bad = "the minimum SOC must be 0-100 %";
    } else if (s.grid_setpoint_w < -HARD_MAX_W || s.grid_setpoint_w > HARD_MAX_W) {
        bad = "the grid setpoint must be within -5000..+5000 W";
    } else if (!limit_ok(s.max_charge_w) || !limit_ok(s.max_discharge_w) || !limit_ok(s.max_feed_in_w) ||
               !limit_ok(s.max_import_w)) {
        bad = "limits must be -1 (none) or 0-5000 W";
    } else if (s.safety_max_w < 0 || s.safety_max_w > HARD_MAX_W) {
        bad = "the safety cap must be 0-5000 W";
    } else if (!valid_hold_voltage(s.voltage_hold_cv)) {
        bad = "the battery voltage threshold must be 8.00-80.00 V, or disabled";
    } else if (s.tz[0] == 0 || memchr(s.tz, 0, sizeof s.tz) == nullptr) {
        bad = "the time zone must be a POSIX TZ string of up to 47 characters";
    }
    for (const char *c = s.tz; !bad && *c; c++) {
        if (*c <= 0x20 || *c >= 0x7F || *c == '"' || *c == '\\') {
            bad = "the time zone must be a POSIX TZ string without spaces, quotes or backslashes";
        }
    }
    for (int i = 0; !bad && i < SCHEDULES; i++) {
        const Schedule &sc = s.schedule[i];
        if (sc.day > DAY_MONTHLY) {
            bad = "a schedule's day must be 0-10";
        } else if (sc.start_s < 0 || sc.start_s >= 86400) {
            bad = "a schedule's start must be 00:00-23:59";
        } else if (sc.duration_s < 0 || sc.duration_s > 86400) {
            bad = "a schedule's duration must be 0-24 h";
        } else if (sc.soc > 100) {
            bad = "a schedule's SOC must be 0-100 %";
        }
    }
    if (bad && cap) snprintf(why, cap, "%s", bad);
    return bad == nullptr;
}

}  // namespace ess
