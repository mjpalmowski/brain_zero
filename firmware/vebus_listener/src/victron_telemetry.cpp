#include "victron_telemetry.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace gx {
namespace {
enum Field { TEXT, SERIAL, VERSION, HEARTBEAT, SYSTEM_SOC, SYSTEM_V, EXTERNAL_SOC, EXTERNAL_V,
             DC_V, DC_A, DC_W, IN_V, IN_A, IN_W, IN_HZ, OUT_V, OUT_A, OUT_W, OUT_HZ };
struct Topic { const char *path; Field field; const char *text; };
const Topic TOPICS[] = {
    {"system/0/Serial", SERIAL, nullptr},
    {"system/0/ProductName", TEXT, "brain_zero"},
    {"system/0/CustomName", TEXT, "brain_zero"},
    {"system/0/Manufacturer", TEXT, "mjpalmowski"},
    {"system/0/FirmwareVersion", VERSION, nullptr},
    // Our actual dated firmware release, not a claimed Venus OS version. The
    // HA client also uses this version to enable its stale-metric maintenance.
    {"platform/0/Firmware/Installed/Version", TEXT, "v2026.11.1~brain_zero-telemetry"},
    {"vebus/288/ProductName", TEXT, "VE.Bus inverter/charger"},
    {"vebus/288/CustomName", TEXT, "MultiPlus-II (brain_zero)"},
    {"vebus/288/Manufacturer", TEXT, "Victron Energy"},
    {"battery/0/ProductName", TEXT, "External battery"},
    {"battery/0/CustomName", TEXT, "External battery (brain_zero)"},
    {"battery/0/Manufacturer", TEXT, "External"},
    {"heartbeat", HEARTBEAT, nullptr},
    {"system/0/Dc/Battery/Soc", SYSTEM_SOC, nullptr},
    {"system/0/Dc/Battery/Voltage", SYSTEM_V, nullptr},
    {"battery/0/Soc", EXTERNAL_SOC, nullptr},
    {"battery/0/Dc/0/Voltage", EXTERNAL_V, nullptr},
    {"vebus/288/Dc/0/Voltage", DC_V, nullptr},
    {"vebus/288/Dc/0/Current", DC_A, nullptr},
    {"vebus/288/Dc/0/Power", DC_W, nullptr},
    {"vebus/288/Ac/ActiveIn/L1/V", IN_V, nullptr},
    {"vebus/288/Ac/ActiveIn/L1/I", IN_A, nullptr},
    {"vebus/288/Ac/ActiveIn/L1/P", IN_W, nullptr},
    {"vebus/288/Ac/ActiveIn/L1/F", IN_HZ, nullptr},
    {"vebus/288/Ac/Out/L1/V", OUT_V, nullptr},
    {"vebus/288/Ac/Out/L1/I", OUT_A, nullptr},
    {"vebus/288/Ac/Out/L1/P", OUT_W, nullptr},
    {"vebus/288/Ac/Out/L1/F", OUT_HZ, nullptr},
};
constexpr int COUNT = sizeof TOPICS / sizeof TOPICS[0];

bool quote(const char *s, char *out, size_t cap) {
    size_t n = 0;
    auto put = [&](char c) { if (n + 1 >= cap) return false; out[n++] = c; return true; };
    if (!put('"')) return false;
    for (; *s; ++s) {
        unsigned char c = *s;
        if (c < 32 || c > 126) return false;
        if ((c == '"' || c == '\\') && !put('\\')) return false;
        if (!put(c)) return false;
    }
    if (!put('"')) return false;
    out[n] = 0; return true;
}
bool fit(int n, size_t cap) { return n >= 0 && size_t(n) < cap; }
} // namespace

void identity(const uint8_t mac[6], const char *firmware, Identity &out) {
    snprintf(out.installation, sizeof out.installation, "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(out.firmware, sizeof out.firmware, "%s", firmware);
}
int topic_count() { return COUNT; }
int sensor_count() { return 17; }

bool read_request(const Identity &id, const char *topic, Request &out) {
    char prefix[24]; snprintf(prefix, sizeof prefix, "R/%s/", id.installation);
    size_t n = strlen(prefix);
    if (strncmp(topic, prefix, n)) return false;
    const char *path = topic + n;
    if (strlen(path) >= sizeof out.path || strchr(path, '#') || strchr(path, '+') || strstr(path, "//")) return false;
    for (const char *p = path; *p; ++p) if ((unsigned char)*p < 33 || (unsigned char)*p > 126) return false;
    Request r;
    r.keepalive = !strcmp(path, "keepalive");
    if (!r.keepalive) snprintf(r.path, sizeof r.path, "%s", path);
    bool found = r.keepalive;
    for (int i = 0; i < COUNT && !found; ++i) found = matches(r, i);
    if (!found) return false;
    out = r; return true;
}
bool matches(const Request &r, int index) {
    if (index < 0 || index >= COUNT) return false;
    size_t n = strlen(r.path);
    return !n || (!strncmp(TOPICS[index].path, r.path, n) &&
                  (TOPICS[index].path[n] == 0 || TOPICS[index].path[n] == '/'));
}

bool notification(int index, const Identity &id, const Snapshot &s,
                  char *topic, size_t tcap, char *payload, size_t pcap) {
    if (index < 0 || index >= COUNT || !tcap || !pcap) return false;
    const Topic &t = TOPICS[index];
    if (!fit(snprintf(topic, tcap, "N/%s/%s", id.installation, t.path), tcap)) return false;
    auto fresh = [&](const vebus::Reading &r) {
        return s.bus.bus_fresh(s.now) && r.t >= 0 && s.now >= r.t && s.now-r.t < 10000000 && isfinite(r.v);
    };
    auto value = [&](const vebus::Reading &r) { return fresh(r) ? r.v : NAN; };
    const bool external_soc = s.external_soc_valid && isfinite(s.external_soc) && s.external_soc >= 0 && s.external_soc <= 100;
    const bool external_voltage = s.external_voltage_valid && isfinite(s.external_voltage) &&
                                  s.external_voltage >= 3 && s.external_voltage <= 80;
    float v = NAN;
    switch (t.field) {
    case TEXT: case SERIAL: case VERSION: {
        char text[128];
        const char *str = t.field == TEXT ? t.text : t.field == SERIAL ? id.installation : id.firmware;
        if (!quote(str, text, sizeof text)) return false;
        return fit(snprintf(payload, pcap, "{\"value\":%s}", text), pcap);
    }
    case HEARTBEAT:
        return fit(snprintf(payload, pcap, "{\"value\":%lu}", (unsigned long)s.utc), pcap);
    case SYSTEM_SOC: v = external_soc ? s.external_soc : value(s.bus.soc); break;
    case SYSTEM_V: v = external_voltage ? s.external_voltage : value(s.bus.bat_v); break;
    case EXTERNAL_SOC: v = external_soc ? s.external_soc : NAN; break;
    case EXTERNAL_V: v = external_voltage ? s.external_voltage : NAN; break;
    case DC_V: v = value(s.bus.bat_v); break;
    case DC_A: v = value(s.bus.bat_a); break;
    case DC_W: v = value(s.bus.bat_v) * value(s.bus.bat_a); break;
    case IN_V: v = value(s.bus.mains_v); break;
    case IN_A: v = value(s.bus.mains_a); break;
    case IN_W: v = -value(s.bus.acin_w); break; // VE.Bus snapshot +export -> GX +import
    case IN_HZ: v = value(s.bus.mains_v) > 50 ? value(s.bus.ac_hz) : NAN; break;
    case OUT_V: v = value(s.bus.inverter_v); break;
    case OUT_A: v = value(s.bus.inverter_a); break;
    case OUT_W: v = value(s.bus.acout_w); break;
    case OUT_HZ: v = value(s.bus.ac_hz); break;
    }
    return fit(isfinite(v) ? snprintf(payload, pcap, "{\"value\":%.3f}", v) :
                           snprintf(payload, pcap, "{\"value\":null}"), pcap);
}
bool completion(const Identity &id, const Request &r, uint32_t utc,
                char *topic, size_t tcap, char *payload, size_t pcap) {
    if (!fit(snprintf(topic, tcap, "N/%s/full_publish_completed", id.installation), tcap)) return false;
    if (!r.echo[0]) return fit(snprintf(payload, pcap, "{\"value\":%lu}", (unsigned long)utc), pcap);
    char echo[260]; if (!quote(r.echo, echo, sizeof echo)) return false;
    return fit(snprintf(payload, pcap, "{\"value\":%lu,\"full-publish-completed-echo\":%s}",
                        (unsigned long)utc, echo), pcap);
}
} // namespace gx
