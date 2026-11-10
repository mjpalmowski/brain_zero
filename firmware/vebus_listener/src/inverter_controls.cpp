#include "inverter_controls.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace inverter {
const char *mode_name(int m) {
    static const char *const names[] = {"Off", "Charger only", "Inverter only", "On"};
    return m >= 4 && m <= 7 ? names[m - 4] : "Unknown";
}
const char *charge_name(int s) {
    static const char *const names[] = {"Initializing", "Bulk", "Absorption", "Float", "Storage",
                                       "Repeated absorption", "Forced absorption", "Equalization", "Bulk stopped"};
    return s >= 0 && s <= 8 ? names[s] : "Unknown";
}
const char *progress_name(Progress p) {
    static const char *const names[] = {"idle", "queued", "sending", "verifying", "confirmed", "failed"};
    return names[(unsigned)p];
}
bool command_key(const char *k) {
    return !strcmp(k, "inverter_mode") || !strcmp(k, "ac_input_limit") || !strcmp(k, "equalize") ||
           !strcmp(k, "stop_equalize_absorption") || !strcmp(k, "stop_equalize_float") ||
           !strcmp(k, "restart_vebus") || !strcmp(k, "redetect_vebus");
}
bool parse(const char *k, const char *value, Command &out, char *why, size_t cap) {
    auto fail = [&](const char *s) { if (cap) snprintf(why, cap, "%s", s); return false; };
    Command c;
    if (!strcmp(k, "inverter_mode")) {
        c.action = Action::MODE;
        for (int m = 4; m <= 7; m++) if (!strcmp(value, mode_name(m))) c.value = m;
        if (!c.value) return fail("Choose Off, Charger only, Inverter only or On");
    } else if (!strcmp(k, "ac_input_limit")) {
        char *end = nullptr;
        double x = strtod(value, &end);
        if (!*value || *end || !isfinite(x) || x < 0 || x > 50 || fabs(x * 10 - round(x * 10)) > 0.00001)
            return fail("Input current must be 0 to 50 A in 0.1 A steps");
        c.action = Action::CURRENT;
        c.value = (int)lround(x * 10);
    } else {
        if (!strcmp(k, "equalize")) c.action = Action::EQUALIZE;
        else if (!strcmp(k, "stop_equalize_absorption")) c.action = Action::ABSORPTION;
        else if (!strcmp(k, "stop_equalize_float")) c.action = Action::FLOAT;
        else if (!strcmp(k, "restart_vebus")) c.action = Action::RESTART;
        else if (!strcmp(k, "redetect_vebus")) c.action = Action::REDETECT;
        else return fail("Unknown inverter control");
        if (strcmp(value, "PRESS")) return fail("Send PRESS to run this action");
    }
    out = c;
    return true;
}
View view(const vebus::State &s, const Job &job, bool master, int64_t now) {
    View v;
    v.job = job;
    auto fresh = [&](int64_t t) { return t >= 0 && now >= t && now - t < 5000000 && s.bus_fresh(now); };
    bool led = fresh(s.led_t);
    if (fresh(s.actual_mode_t)) v.mode = s.actual_mode;
    if (fresh(s.limit_a.t)) v.current = s.limit_a.v;
    if (led && s.ac_input_config >= 0 && !(s.ac_input_config & 8)) {
        v.minimum = s.limit_min_a.v;
        v.maximum = s.limit_max_a.v < 50 ? s.limit_max_a.v : 50; // this project's 50 A MultiPlus-II
    }
    if (fresh(s.charge_state_t)) {
        v.main_state = s.charge_main_state;
        if (v.main_state == 9) v.charge_state = s.charge_state;
    }
    v.available = master && s.bus_fresh(now) && !job.active();
    v.mode_adjustable = v.available && v.mode >= 4 && led && !(s.ac_input_config & 8);
    v.current_adjustable = v.available && v.minimum >= 0 && v.maximum >= v.minimum &&
                           (s.ac_input_config & 4) && v.current >= 0;
    v.can_equalize = v.available && v.main_state == 9 && v.charge_state >= 2 && v.charge_state <= 6;
    v.can_interrupt = v.available && v.main_state == 9 && v.charge_state == 7;
    snprintf(v.reason, sizeof v.reason, "%s", !master ? "S3 control is unavailable: check the controller and other bus masters"
             : !s.bus_fresh(now) ? "No recent inverter data" : job.active() ? "Waiting for the current command to finish"
             : !led ? "Waiting for switch and current-limit data" : (s.ac_input_config & 8) ? "A Digital Multi Control has priority"
             : !(s.ac_input_config & 4) ? "Current limit is fixed by the inverter configuration" : "Ready");
    return v;
}
bool validate(const Command &c, const View &v, char *why, size_t cap) {
    const char *bad = !v.available ? v.reason : nullptr;
    if (!bad) switch (c.action) {
    case Action::MODE:
        if (c.value < 4 || c.value > 7) bad = "Invalid operating mode";
        else if (!v.mode_adjustable) bad = "Remote mode control is unavailable";
        break;
    case Action::CURRENT:
        if (!v.current_adjustable) bad = "Remote current-limit adjustment is unavailable";
        else if (c.value < 0 || c.value > 500 || c.value / 10.0f > v.maximum ||
                 (c.value != 0 && c.value / 10.0f < v.minimum)) bad = "Current limit is outside the inverter's reported range (or zero)";
        break;
    case Action::EQUALIZE: if (!v.can_equalize) bad = "Equalization requires a ready charger outside bulk"; break;
    case Action::ABSORPTION:
    case Action::FLOAT: if (!v.can_interrupt) bad = "Equalization is not active"; break;
    case Action::RESTART:
    case Action::REDETECT: break;
    default: bad = "Unknown inverter control";
    }
    if (bad && cap) snprintf(why, cap, "%s", bad);
    return bad == nullptr;
}
size_t to_json(const View &v, char *out, size_t cap) {
    // All strings here are internal fixed messages; no request text is interpolated.
    int n = snprintf(out, cap,
        "{\"available\":%s,\"mode_adjustable\":%s,\"current_adjustable\":%s,\"can_equalize\":%s,\"can_interrupt\":%s,"
        "\"mode\":%d,\"mode_name\":\"%s\",\"current_a\":%.1f,\"minimum_a\":%.1f,\"maximum_a\":%.1f,"
        "\"main_state\":%d,\"charge_state\":%d,\"charge_name\":\"%s\",\"reason\":\"%s\","
        "\"command\":{\"state\":\"%s\",\"action\":%u,\"value\":%d,\"sends\":%d,\"message\":\"%s\"}}",
        v.available ? "true" : "false", v.mode_adjustable ? "true" : "false", v.current_adjustable ? "true" : "false",
        v.can_equalize ? "true" : "false", v.can_interrupt ? "true" : "false", v.mode, mode_name(v.mode),
        v.current, v.minimum, v.maximum, v.main_state, v.charge_state, charge_name(v.charge_state), v.reason,
        progress_name(v.job.progress), (unsigned)v.job.command.action, v.job.command.value, v.job.sends, v.job.message);
    if (n < 0 || (size_t)n >= cap) { if (cap) out[0] = 0; return 0; }
    return (size_t)n;
}
} // namespace inverter
