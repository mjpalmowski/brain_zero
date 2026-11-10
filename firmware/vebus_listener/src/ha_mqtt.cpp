#include "ha_mqtt.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace ha {

namespace {

enum Kind : uint8_t { SENSOR, BINARY, SWITCH, NUMBER, SELECT, BUTTON };
enum Category : uint8_t { PRIMARY, CONFIG, DIAGNOSTIC };
const char *const COMPONENT[] = {"sensor", "binary_sensor", "switch", "number", "select", "button"};

struct Entity {
    Kind kind;
    const char *key;           // in the state message, the discovery topic and the command topic
    const char *name;
    const char *unit;
    const char *device_class;
    const char *state_class;
    Category category;
    float min, max, step;      // numbers only
};

// The select's options, in ess::Mode's order.
const char *const MODES[] = {"Optimized with BatteryLife", "Optimized without BatteryLife", "Keep batteries charged",
                             "External control"};
// The Multi's state (info frame byte 4), as the dashboard names it.
const char *const MULTI_STATES[] = {"Down",        "Startup",     "Off",          "Slave",  "Invert full",
                                    "Invert half", "Invert AES",  "Power assist", "Bypass", "Charge"};

const Entity ENTITIES[] = {
    {SELECT, "inverter_mode", "Inverter operating mode", nullptr, nullptr, nullptr, PRIMARY, 0, 0, 0},
    {NUMBER, "ac_input_limit", "AC input current limit control", "A", "current", nullptr, PRIMARY, 0, 50, 0.1f},
    {SENSOR, "charge_stage", "Charger stage", nullptr, nullptr, nullptr, PRIMARY, 0, 0, 0},
    {SENSOR, "inverter_command", "Inverter command result", nullptr, nullptr, nullptr, DIAGNOSTIC, 0, 0, 0},
    {BUTTON, "equalize", "Start equalization", nullptr, nullptr, nullptr, CONFIG, 0, 0, 0},
    {BUTTON, "stop_equalize_absorption", "Interrupt equalization to absorption", nullptr, nullptr, nullptr, CONFIG, 0, 0, 0},
    {BUTTON, "stop_equalize_float", "Interrupt equalization to float", nullptr, nullptr, nullptr, CONFIG, 0, 0, 0},
    {BUTTON, "restart_vebus", "Restart VE.Bus system", nullptr, "restart", nullptr, CONFIG, 0, 0, 0},
    {BUTTON, "redetect_vebus", "Redetect VE.Bus system", nullptr, nullptr, nullptr, CONFIG, 0, 0, 0},
    // the Multi, as heard on the bus
    {SENSOR, "grid_w", "Grid power", "W", "power", "measurement", PRIMARY, 0, 0, 0},
    {SENSOR, "acout_w", "AC-out power", "W", "power", "measurement", PRIMARY, 0, 0, 0},
    {SENSOR, "acout_v", "AC-out voltage", "V", "voltage", "measurement", PRIMARY, 0, 0, 0},
    {SENSOR, "inverter_w", "Inverter power", "W", "power", "measurement", PRIMARY, 0, 0, 0},
    {SENSOR, "battery_w", "Battery power", "W", "power", "measurement", PRIMARY, 0, 0, 0},
    {SENSOR, "battery_v", "Battery voltage", "V", "voltage", "measurement", PRIMARY, 0, 0, 0},
    {SENSOR, "battery_a", "Battery current", "A", "current", "measurement", PRIMARY, 0, 0, 0},
    {SENSOR, "soc", "State of charge", "%", "battery", "measurement", PRIMARY, 0, 0, 0},
    {SENSOR, "mains_v", "Mains voltage", "V", "voltage", "measurement", PRIMARY, 0, 0, 0},
    {SENSOR, "mains_a", "AC-in current", "A", "current", "measurement", PRIMARY, 0, 0, 0},
    {SENSOR, "mains_hz", "AC frequency", "Hz", "frequency", "measurement", PRIMARY, 0, 0, 0},
    {SENSOR, "input_limit_a", "Input current limit", "A", "current", "measurement", PRIMARY, 0, 0, 0},
    {SENSOR, "multi_state", "Multi state", nullptr, nullptr, nullptr, PRIMARY, 0, 0, 0},
    {SENSOR, "ess_setpoint_w", "ESS setpoint", "W", "power", "measurement", PRIMARY, 0, 0, 0},
    {BINARY, "ess_live", "ESS live", nullptr, "running", nullptr, PRIMARY, 0, 0, 0},
    {BINARY, "setpoint_fresh", "ESS setpoint acknowledged", nullptr, nullptr, nullptr, PRIMARY, 0, 0, 0},
    {BINARY, "discharge_stopped", "Discharge disabled", nullptr, nullptr, nullptr, PRIMARY, 0, 0, 0},
    {SENSOR, "flag_word", "ESS flag word", nullptr, nullptr, nullptr, DIAGNOSTIC, 0, 0, 0},
    // ESS from this ESP32: its settings, as on the ESS settings page
    {SWITCH, "run", "Run ESS from this ESP32", nullptr, nullptr, nullptr, PRIMARY, 0, 0, 0},
    {SELECT, "mode", "ESS mode", nullptr, nullptr, nullptr, PRIMARY, 0, 0, 0},
    {NUMBER, "grid_setpoint", "Grid setpoint", "W", "power", nullptr, PRIMARY, -5000, 5000, 1},
    {NUMBER, "min_soc", "Minimum SOC", "%", "battery", nullptr, PRIMARY, 0, 100, 1},
    {NUMBER, "max_charge", "Charge limit", "W", "power", nullptr, CONFIG, -1, 5000, 1},
    {NUMBER, "max_discharge", "Inverter limit", "W", "power", nullptr, CONFIG, -1, 5000, 1},
    {NUMBER, "max_feed_in", "Feed-in limit", "W", "power", nullptr, CONFIG, -1, 5000, 1},
    {NUMBER, "max_import", "Grid import limit", "W", "power", nullptr, CONFIG, -1, 5000, 1},
    {SWITCH, "always_peak_shave", "Peak shaving below minimum SOC", nullptr, nullptr, nullptr, CONFIG, 0, 0, 0},
    {SWITCH, "ext_no_charge", "External control: no charge", nullptr, nullptr, nullptr, CONFIG, 0, 0, 0},
    {SWITCH, "ext_no_feed_in", "External control: no feed-in", nullptr, nullptr, nullptr, CONFIG, 0, 0, 0},
    // what the settings decide
    {SENSOR, "ess_state", "ESS state", nullptr, nullptr, nullptr, PRIMARY, 0, 0, 0},
    {SENSOR, "ess_why", "ESS decision", nullptr, nullptr, nullptr, PRIMARY, 0, 0, 0},
    {SENSOR, "ess_target_w", "ESS target", "W", "power", "measurement", PRIMARY, 0, 0, 0},
    {SENSOR, "active_soc_limit", "Active SOC limit", "%", nullptr, "measurement", PRIMARY, 0, 0, 0},
    {SENSOR, "charge_window", "Charge window", nullptr, nullptr, nullptr, PRIMARY, 0, 0, 0},
    {SENSOR, "controller", "Controller", nullptr, nullptr, nullptr, DIAGNOSTIC, 0, 0, 0},
    {SENSOR, "safety_cap", "Safety cap", "W", "power", nullptr, DIAGNOSTIC, 0, 0, 0},
    // the listener itself
    {SENSOR, "rssi", "Wi-Fi signal", "dBm", "signal_strength", "measurement", DIAGNOSTIC, 0, 0, 0},
    {SENSOR, "uptime_s", "Uptime", "s", "duration", "measurement", DIAGNOSTIC, 0, 0, 0},
    {SENSOR, "bus_bad", "Bad bus frames", nullptr, nullptr, "total_increasing", DIAGNOSTIC, 0, 0, 0},
};
constexpr int N = sizeof ENTITIES / sizeof ENTITIES[0];

// Appends to a fixed buffer, keeping it terminated; full() tells if anything was cut.
struct Out {
    char *p;
    size_t cap, n = 0;
    bool cut = false;
    Out(char *buf, size_t size) : p(buf), cap(size) {
        if (cap) p[0] = 0;
    }
    void add(const char *fmt, ...) {
        if (cut || n + 1 >= cap) {
            cut = true;
            return;
        }
        va_list a;
        va_start(a, fmt);
        int k = vsnprintf(p + n, cap - n, fmt, a);
        va_end(a);
        if (k < 0 || (size_t)k >= cap - n) {
            cut = true;
            n = cap - 1;
        } else {
            n += (size_t)k;
        }
    }
    void str(const char *s) {  // a JSON string
        add("\"");
        for (; *s; s++) {
            unsigned char c = (unsigned char)*s;
            if (c == '"' || c == '\\') {
                add("\\%c", c);
            } else if (c < 0x20) {
                add("\\u%04X", c);
            } else {
                add("%c", c);
            }
        }
        add("\"");
    }
};

// Only letters, digits, '_', '-', and '/' between parts: safe in topics and in JSON unescaped.
bool topic_part_ok(const char *s, size_t cap, bool slashes) {
    if (!memchr(s, 0, cap) || !s[0] || s[0] == '/') return false;
    for (const char *c = s; *c; c++) {
        bool ok = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_' ||
                  *c == '-' || (slashes && *c == '/' && c[1] != '/' && c[1] != 0);
        if (!ok) return false;
    }
    return true;
}

bool printable(const char *s, size_t cap, bool spaces) {
    if (!memchr(s, 0, cap)) return false;
    for (const char *c = s; *c; c++) {
        if (*c < (spaces ? 0x20 : 0x21) || *c > 0x7E) return false;
    }
    return true;
}

}  // namespace

bool validate(const Settings &s, char *why, size_t cap) {
    const char *bad = nullptr;
    if (!printable(s.host, sizeof s.host, false) || (s.enabled && !s.host[0])) {
        bad = "the broker must be a host name or address";
    } else if (s.port == 0) {
        bad = "the port must be 1-65535";
    } else if (!printable(s.user, sizeof s.user, false) || !printable(s.password, sizeof s.password, true)) {
        bad = "the user name and password must be plain ASCII (no spaces in the user name)";
    } else if (!topic_part_ok(s.base, sizeof s.base, true)) {
        bad = "the base topic may hold letters, digits, '_', '-' and '/' between parts";
    } else if (!topic_part_ok(s.prefix, sizeof s.prefix, true)) {
        bad = "the discovery prefix may hold letters, digits, '_', '-' and '/' between parts";
    } else if (s.interval_s < 1 || s.interval_s > 3600) {
        bad = "the interval must be 1-3600 s";
    }
    if (bad && cap) snprintf(why, cap, "%s", bad);
    return bad == nullptr;
}

int entity_count() { return N; }

bool discovery(int i, const Settings &s, const Ids &ids, bool remove, char *topic, size_t tcap, char *payload,
               size_t pcap) {
    if (i < 0 || i >= N || pcap == 0) return false;
    const Entity &e = ENTITIES[i];
    int t = snprintf(topic, tcap, "%s/%s/%s/%s/config", s.prefix, COMPONENT[e.kind], ids.node, e.key);
    if (t < 0 || (size_t)t >= tcap) return false;
    Out o(payload, pcap);
    if (remove) return true;  // an empty retained message deletes the entity
    o.add("{\"name\":");
    o.str(e.name);
    o.add(",\"unique_id\":\"%s_%s\",\"availability_topic\":\"%s/status\"", ids.node, e.key, s.base);
    if (e.kind != BUTTON) o.add(",\"state_topic\":\"%s/state\",\"value_template\":\"{{ value_json.%s }}\"", s.base, e.key);
    if (e.kind == SWITCH || e.kind == NUMBER || e.kind == SELECT || e.kind == BUTTON)
        o.add(",\"command_topic\":\"%s/%s/set\"", s.base, e.key);
    if (e.kind == BUTTON) o.add(",\"payload_press\":\"PRESS\",\"retain\":false,\"enabled_by_default\":false");
    if (e.unit) o.add(",\"unit_of_measurement\":\"%s\"", e.unit);
    if (e.device_class) o.add(",\"device_class\":\"%s\"", e.device_class);
    if (e.state_class) o.add(",\"state_class\":\"%s\"", e.state_class);
    if (e.category != PRIMARY) o.add(",\"entity_category\":\"%s\"", e.category == CONFIG ? "config" : "diagnostic");
    if (e.kind == NUMBER) o.add(",\"min\":%g,\"max\":%g,\"step\":%g,\"mode\":\"box\"", e.min, e.max, e.step);
    if (e.kind == SELECT) {
        o.add(",\"options\":[");
        for (int k = 0; k < 4; k++) o.add("%s\"%s\"", k ? "," : "", !strcmp(e.key, "inverter_mode") ? inverter::mode_name(k + 4) : MODES[k]);
        o.add("]");
    }
    if (e.kind == SENSOR || e.kind == BINARY) {
        unsigned expire = 6u * s.interval_s;  // a few missed messages and it shows unavailable
        o.add(",\"expire_after\":%u", expire < 30 ? 30 : expire);
    }
    o.add(",\"device\":{\"identifiers\":[\"%s\"],\"name\":\"brain_zero\",\"manufacturer\":\"mjpalmowski\""
          ",\"model\":\"ESP32-S3 VE.Bus master for a MultiPlus-II\"",
          ids.node);
    if (ids.version[0]) {
        o.add(",\"sw_version\":");
        o.str(ids.version);
    }
    if (ids.url[0]) {
        o.add(",\"configuration_url\":");
        o.str(ids.url);
    }
    o.add("}}");
    return !o.cut;
}

size_t state_json(const vebus::State &st, const web::EssView &v, int64_t now, const Extra &x, char *out, size_t cap) {
    Out o(out, cap);
    bool first = true;
    auto key = [&](const char *k) {
        o.add("%s\"%s\":", first ? "{" : ",", k);
        first = false;
    };
    auto fresh = [&](int64_t t, int max_s) { return t != vebus::NEVER && now - t <= (int64_t)max_s * 1000000; };
    auto num = [&](const char *k, bool ok, double value, int decimals) {
        key(k);
        if (ok) {
            o.add("%.*f", decimals, value + 0.0);  // + 0.0 turns -0 into 0
        } else {
            o.add("null");
        }
    };
    auto text = [&](const char *k, const char *value) {
        key(k);
        if (value) {
            o.str(value);
        } else {
            o.add("null");
        }
    };
    auto on_off = [&](const char *k, int value) { text(k, value < 0 ? nullptr : value ? "ON" : "OFF"); };

    // the Multi
    bool bus = st.bus_fresh(now);
    text("inverter_mode", bus && fresh(st.actual_mode_t, 5) && st.actual_mode >= 4 && st.actual_mode <= 7 ? inverter::mode_name(st.actual_mode) : nullptr);
    num("ac_input_limit", bus && fresh(st.limit_a.t, 5), st.limit_a.v, 1);
    text("charge_stage", bus && fresh(st.charge_state_t, 5) && st.charge_main_state == 9 ? inverter::charge_name(st.charge_state) : nullptr);
    text("inverter_command", v.inverter.job.message);
    num("grid_w", fresh(st.acin_w.t, 10), -st.acin_w.v, 0);
    num("acout_w", fresh(st.acout_w.t, 10), st.acout_w.v, 0);
    num("acout_v", bus && fresh(st.inverter_v.t, 3), st.inverter_v.v, 2);
    num("inverter_w", fresh(st.inverter_w.t, 10), st.inverter_w.v, 0);
    bool dc = fresh(st.bat_v.t, 10) && fresh(st.bat_a.t, 10);
    num("battery_w", dc, st.bat_v.v * st.bat_a.v, 0);
    num("battery_v", fresh(st.bat_v.t, 10), st.bat_v.v, 2);
    num("battery_a", fresh(st.bat_a.t, 10), st.bat_a.v, 1);
    num("soc", fresh(st.soc.t, 60), st.soc.v, 1);
    num("mains_v", fresh(st.mains_v.t, 10), st.mains_v.v, 1);
    num("mains_a", fresh(st.mains_a.t, 10), st.mains_a.v, 2);
    num("mains_hz", fresh(st.ac_hz.t, 10), st.ac_hz.v, 2);
    num("input_limit_a", fresh(st.limit_a.t, 30), st.limit_a.v, 1);
    bool known = fresh(st.device_state_t, 10) && st.device_state >= 0 && st.device_state <= 9;
    text("multi_state", known ? MULTI_STATES[st.device_state] : nullptr);
    num("ess_setpoint_w", fresh(st.setpoint_w.t, 15), -st.setpoint_w.v, 0);
    on_off("ess_live", -1);  // retained entity: the Multi's operating state is unconfirmed
    on_off("setpoint_fresh", st.setpoint_fresh(now));
    on_off("discharge_stopped", st.discharge_disabled(now));
    num("flag_word", fresh(st.flags.t, 30), st.flags.v, 0);
    // ESS from this ESP32
    const ess::Settings &s = v.settings;
    const ess::Decision &d = v.decision;
    on_off("run", s.enabled);
    int mode = (int)s.mode;
    text("mode", mode >= 0 && mode < 4 ? MODES[mode] : nullptr);
    num("grid_setpoint", true, s.grid_setpoint_w, 0);
    num("min_soc", true, s.min_soc, 0);
    num("max_charge", true, s.max_charge_w, 0);
    num("max_discharge", true, s.max_discharge_w, 0);
    num("max_feed_in", true, s.max_feed_in_w, 0);
    num("max_import", true, s.max_import_w, 0);
    on_off("always_peak_shave", s.always_peak_shave);
    on_off("ext_no_charge", s.ext_no_charge);
    on_off("ext_no_feed_in", s.ext_no_feed_in);
    text("ess_state", ess::state_name(d.state));
    text("ess_why", d.why);
    num("ess_target_w", true, -d.target_w, 0);
    num("active_soc_limit", d.active_soc_limit >= 0, d.active_soc_limit, 0);
    char window[8];
    snprintf(window, sizeof window, "%d", d.schedule + 1);
    text("charge_window", d.schedule >= 0 ? window : "none");
    text("controller", v.ctl);
    num("safety_cap", true, s.safety_max_w, 0);
    // the listener
    num("rssi", x.rssi != 0, x.rssi, 0);
    num("uptime_s", true, x.uptime_s, 0);
    num("bus_bad", true, st.bad, 0);
    o.add("}");
    return o.cut ? 0 : o.n;
}

bool command_key(const Settings &s, const char *topic, size_t len, char *key, size_t cap) {
    size_t b = strlen(s.base);
    if (len < b + 6 || memcmp(topic, s.base, b) != 0 || topic[b] != '/' || memcmp(topic + len - 4, "/set", 4) != 0) {
        return false;
    }
    const char *k = topic + b + 1;
    size_t kl = len - b - 5;
    if (kl == 0 || kl >= cap || memchr(k, '/', kl)) return false;
    memcpy(key, k, kl);
    key[kl] = 0;
    return true;
}

bool apply_command(const char *key, const char *payload, ess::Settings &s, char *why, size_t cap) {
    char value[48];
    size_t n = strlen(payload);
    while (n && (payload[n - 1] == ' ' || payload[n - 1] == '\n' || payload[n - 1] == '\r')) n--;
    while (n && *payload == ' ') {
        payload++;
        n--;
    }
    if (n >= sizeof value) n = sizeof value - 1;
    memcpy(value, payload, n);
    value[n] = 0;
    auto fail = [&](const char *msg) -> bool {
        if (cap) snprintf(why, cap, "%s: %s", key, msg);
        return false;
    };
    auto on_off = [&](bool *b) -> bool {
        if (!strcmp(value, "ON") || !strcmp(value, "1")) {
            *b = true;
        } else if (!strcmp(value, "OFF") || !strcmp(value, "0")) {
            *b = false;
        } else {
            return fail("send ON or OFF");
        }
        return true;
    };
    auto number = [&](double lo, double hi, double *x) -> bool {
        char *end = nullptr;
        double v = strtod(value, &end);
        if (!value[0] || *end || !(v >= lo && v <= hi)) return false;
        *x = v;
        return true;
    };
    auto limit = [&](int *w) -> bool {  // -1 for no limit, or 0-5000 W
        double x;
        if (!number(-1, ess::HARD_MAX_W, &x) || (x < 0 && x != -1)) return fail("send -1 (no limit) or 0-5000");
        *w = (int)lround(x);
        return true;
    };
    double x;
    if (!strcmp(key, "run")) return on_off(&s.enabled);
    if (!strcmp(key, "always_peak_shave")) return on_off(&s.always_peak_shave);
    if (!strcmp(key, "ext_no_charge")) return on_off(&s.ext_no_charge);
    if (!strcmp(key, "ext_no_feed_in")) return on_off(&s.ext_no_feed_in);
    if (!strcmp(key, "mode")) {
        for (int k = 0; k < 4; k++) {
            if (!strcmp(value, MODES[k]) || (value[0] == '0' + k && !value[1])) {
                s.mode = (ess::Mode)k;
                return true;
            }
        }
        return fail("unknown mode");
    }
    if (!strcmp(key, "grid_setpoint")) {
        if (!number(-ess::HARD_MAX_W, ess::HARD_MAX_W, &x)) return fail("send -5000 to 5000");
        s.grid_setpoint_w = (int)lround(x);
        return true;
    }
    if (!strcmp(key, "min_soc")) {
        if (!number(0, 100, &x)) return fail("send 0 to 100");
        s.min_soc = (float)x;
        return true;
    }
    if (!strcmp(key, "max_charge")) return limit(&s.max_charge_w);
    if (!strcmp(key, "max_discharge")) return limit(&s.max_discharge_w);
    if (!strcmp(key, "max_feed_in")) return limit(&s.max_feed_in_w);
    if (!strcmp(key, "max_import")) return limit(&s.max_import_w);
    return fail("not a setting Home Assistant can change");
}

}  // namespace ha
