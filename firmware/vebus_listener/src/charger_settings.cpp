#include "charger_settings.h"
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace charger {
int count(Profile profile) { return profile == Profile::ASSIST ? 2 : COUNT; }
uint8_t setting_id(int i, Profile profile) {
    static const uint8_t ids[] = {4, 2, 3}, assist[] = {0, 48};
    return profile == Profile::ASSIST ? assist[i] : ids[i];
}
const char *key(int i, Profile profile) {
    if (profile == Profile::ASSIST) return i == 0 ? "power_assist" : "boost_factor";
    static const char *const keys[] = {"charge_current", "absorption_voltage", "float_voltage"};
    return keys[i];
}
const char *stage_name(Stage s) {
    static const char *const names[] = {"idle", "reading", "writing", "verifying", "done", "failed"};
    return names[(unsigned)s];
}
// Intersect the live bounds with the envelope for the matched inverter profile.
// The current ceilings apply to the currently verified models in each class.
static bool parameter_bounds(int i, const Parameter &p, Profile profile, uint8_t nominal_voltage,
                             uint16_t &minimum, uint16_t &maximum) {
    if (!p.info || p.offset != 0 || p.access != 0) return false;
    if (profile == Profile::ASSIST && i == 0) {
        minimum = 0;
        maximum = 1;
        return p.scale == 1 && (p.maximum & ASSIST_MASK);
    }
    if (p.minimum > p.maximum) return false;
    if (profile == Profile::ASSIST) {
        minimum = p.minimum;
        maximum = p.maximum;
        return p.scale == -16 && minimum >= 4 && maximum <= 56;
    }

    struct Envelope { uint8_t volts; uint16_t minimum, maximum, max_current; };
    static constexpr Envelope envelopes[] = {
        {12, 1200, 1600, 120},
        {24, 2400, 3200, 120},
        {48, 4800, 6400, 70},
    };
    if (p.scale != (i == CURRENT ? 1 : -100)) return false;
    const Envelope *matched = nullptr;
    for (const Envelope &envelope : envelopes) {
        if (envelope.volts == nominal_voltage) matched = &envelope;
        // A range spanning another supported voltage class is ambiguous.
        else if (i != CURRENT && p.minimum <= envelope.maximum && p.maximum >= envelope.minimum) return false;
    }
    if (!matched) return false;
    const uint16_t floor = i == CURRENT ? 0 : matched->minimum;
    const uint16_t ceiling = i == CURRENT ? matched->max_current : matched->maximum;
    minimum = p.minimum > floor ? p.minimum : floor;
    maximum = p.maximum < ceiling ? p.maximum : ceiling;
    return minimum <= maximum;
}

bool schema_ok(int i, const Parameter &p, Profile profile, uint8_t nominal_voltage) {
    uint16_t minimum, maximum;
    return parameter_bounds(i, p, profile, nominal_voltage, minimum, maximum);
}
bool parse(const char *current, const char *absorption, const char *floating, const char *base,
           Change &out, char *why, size_t cap) {
    auto fail = [&](const char *s) { if (cap) snprintf(why, cap, "%s", s); return false; };
    const char *values[] = {current, absorption, floating};
    Change c;
    for (int i = 0; i < COUNT; i++) {
        char *end = nullptr;
        errno = 0;
        double n = strtod(values[i], &end), units = i == CURRENT ? 1 : 100;
        double raw = n * units;
        double rounded = round(raw);
        if (!values[i][0] || *end || errno || !isfinite(raw) || raw < 0 || rounded > UINT16_MAX ||
            fabs(raw - rounded) > 0.000001)
            return fail(i == CURRENT ? "Enter charge current in whole amps" : "Enter charge voltage in 0.01 V steps");
        c.raw[i] = (uint16_t)rounded;
    }
    const char *p = base;
    for (int i = 0; i < COUNT; i++) {
        if (*p < '0' || *p > '9') return fail("Reload the charger settings before saving");
        char *end = nullptr;
        errno = 0;
        unsigned long n = strtoul(p, &end, 10);
        if (errno || n > 65535 || (i < COUNT - 1 ? *end != ',' : *end != 0))
            return fail("Reload the charger settings before saving");
        c.expected[i] = (uint16_t)n;
        p = end + (i < COUNT - 1 ? 1 : 0);
    }
    out = c;
    return true;
}
bool parse_assist(const char *enabled, const char *boost, const char *base, Change &out, char *why, size_t cap) {
    auto fail = [&](const char *s) { if (cap) snprintf(why, cap, "%s", s); return false; };
    if (strcmp(enabled, "0") && strcmp(enabled, "1")) return fail("PowerAssist must be 0 or 1");
    Change c;
    const char *p = base;
    for (int i = 0; i < 2; i++) {
        if (*p < '0' || *p > '9') return fail("Read PowerAssist settings before saving");
        char *end = nullptr; errno = 0;
        unsigned long n = strtoul(p, &end, 10);
        if (errno || n > 65535 || (i == 0 ? *end != ',' : *end != 0)) return fail("Invalid PowerAssist readback baseline");
        c.expected[i] = (uint16_t)n; p = end + (i == 0 ? 1 : 0);
    }
    char *end = nullptr; errno = 0;
    double value = strtod(boost, &end), raw = value * 16;
    if (!*boost || *end || errno || !isfinite(value) || value < .25 || value > 3.5 || fabs(raw - round(raw)) > .000001)
        return fail("Boost factor must be 0.25 to 3.5 in 0.0625 steps");
    c.raw[0] = (c.expected[0] & ~ASSIST_MASK) | (!strcmp(enabled, "1") ? ASSIST_MASK : 0);
    c.raw[1] = (uint16_t)lround(raw);
    out = c; return true;
}
int plan(const Parameter *p, const Change &c, uint8_t *order, char *why, size_t cap, Profile profile, uint8_t nominal_voltage) {
    auto fail = [&](const char *s) { if (cap) snprintf(why, cap, "%s", s); return -1; };
    for (int i = 0; i < count(profile); i++) {
        uint16_t minimum, maximum;
        if (!parameter_bounds(i, p[i], profile, nominal_voltage, minimum, maximum) || !p[i].known)
            return fail("Unsupported or access-protected stored setting; use VEConfigure");
        if (p[i].raw != c.expected[i]) return fail("Settings changed since this form was loaded; reload and review before saving");
        if (profile == Profile::ASSIST && i == 0) {
            if ((p[0].raw ^ c.raw[0]) & ~ASSIST_MASK) return fail("Only the supported PowerAssist flag may change");
            continue;
        }
        if (c.raw[i] < minimum || c.raw[i] > maximum)
            return fail("A requested value is outside the inverter's supported range");
    }
    if (profile == Profile::ASSIST) {
        int n = 0;
        for (int i = 0; i < 2; i++) if (p[i].raw != c.raw[i]) order[n++] = i;
        return n;
    }
    if (c.raw[FLOAT] > c.raw[ABSORPTION]) return fail("Float voltage must not exceed absorption voltage");
    int n = 0;
    auto add = [&](int i) { if (p[i].raw != c.raw[i]) order[n++] = (uint8_t)i; };
    add(CURRENT);
    if (c.raw[ABSORPTION] < p[FLOAT].raw) { add(FLOAT); add(ABSORPTION); }
    else { add(ABSORPTION); add(FLOAT); }
    return n;
}
size_t to_json(const View &v, char *out, size_t cap) {
    size_t n = 0;
    bool cut = cap == 0;
    auto add = [&](const char *fmt, ...) {
        if (cut) return;
        va_list a; va_start(a, fmt);
        int k = vsnprintf(out + n, cap - n, fmt, a);
        va_end(a);
        if (k < 0 || (size_t)k >= cap - n) cut = true;
        else n += (size_t)k;
    };
    const Status &s = v.status;
    bool assist = s.profile == Profile::ASSIST;
    // Strings originate from internal fixed messages, never request text.
    add("{\"available\":%s,\"reason\":\"%s\",\"state\":\"%s\",\"saving\":%s,\"message\":\"%s\","
        "\"changes\":%d,\"writes\":%d,\"confirmed\":%d,\"nominal_voltage\":",
        v.available ? "true" : "false", v.reason, stage_name(s.stage), s.saving ? "true" : "false", s.message,
        s.changes, s.writes, s.confirmations);
    if (s.nominal_voltage) add("%u", unsigned(s.nominal_voltage)); else add("null");
    add(",\"product_id\":");
    if (s.product_id) add("%u", unsigned(s.product_id)); else add("null");
    add(",\"firmware\":");
    if (s.firmware) add("%lu", (unsigned long)s.firmware); else add("null");
    add(",\"parameters\":[");
    for (int i = 0; i < count(s.profile); i++) {
        const Parameter &p = s.parameter[i];
        double unit = assist ? (i == 0 ? 1 : 16) : i == CURRENT ? 1 : 100;
        uint16_t minimum, maximum;
        bool supported = parameter_bounds(i, p, s.profile, s.nominal_voltage, minimum, maximum);
        bool readable = supported || (!assist && !s.nominal_voltage && p.info &&
                                      p.scale == (i == CURRENT ? 1 : -100) && p.offset == 0);
        bool valid = readable && p.known && p.read_at >= 0 && v.now >= p.read_at && v.now - p.read_at < 120000000;
        add("%s{\"key\":\"%s\",\"id\":%u,\"raw\":%u,\"value\":", i ? "," : "", key(i, s.profile), setting_id(i, s.profile), p.raw);
        if (valid) add("%.4f", assist && i == 0 ? double(bool(p.raw & ASSIST_MASK)) : p.raw / unit); else add("null");
        if (supported) add(",\"minimum\":%.4f,\"maximum\":%.4f", minimum / unit, maximum / unit);
        else add(",\"minimum\":null,\"maximum\":null");
        add(",\"step\":%.4f,\"supported\":%s,\"age_s\":%.1f,\"acknowledged\":%s,\"verified\":%s,\"requested\":",
            1 / unit, supported ? "true" : "false",
            p.read_at < 0 ? -1.0 : (v.now - p.read_at) / 1e6,
            s.acknowledged[i] ? "true" : "false", s.verified[i] ? "true" : "false");
        if (s.saving) add("%.4f", assist && i == 0 ? double(bool(s.requested[i] & ASSIST_MASK)) : s.requested[i] / unit); else add("null");
        add("}");
    }
    add("]}");
    if (cut) { if (cap) out[0] = 0; return 0; }
    return n;
}
} // namespace charger
