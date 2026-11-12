#include "web.h"

#include <Arduino.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <atomic>
#include <cJSON.h>
#include <mdns.h>
#include <memory>
#include <new>
#include <ping/ping_sock.h>
#include <stdarg.h>
#include <time.h>

#include "dashboard.h"
#include "compatibility_profile.h"
#include "console_auth.h"
#include "controller_time.h"
#include "device_page.h"
#include "device_settings.h"
#include "diagnostics.h"
#include "diagnostics_page.h"
#include "esp_system.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "brand_assets.h"
#include "esp_timer.h"
#include "ess_page.h"
#include "firmware_ota.h"
#include "firmware_page.h"
#include "firmware_release_job.h"
#include "ha_mqtt.h"
#include "mcp_server.h"
#include "mqtt_link.h"
#include "mqtt_page.h"
#include "multi_page.h"
#include "inverter_page.h"
#include "requester.h"
#include "rvsc.h"
#include "vebus_state.h"
#include "warning_history.h"
#include "wifi_recovery.h"

#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#endif
#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD ""
#endif

void listener_state(vebus::State &out, faults::Report *report); // cached decoded state and report
const warnings::History &listener_warning_history(); // same Arduino loop as the HTTP handlers
const warnings::Live &listener_warning_status();
void listener_diagnostics(diagnostics::Cache &out); // strictly cached, no queued reads
bool listener_fault_report_read(char *why,size_t cap);
size_t listener_tx_json(char *out, size_t cap);  // main.cpp: "\"tx\":{...},\"ctl\":{...}", our own requests
void listener_ess(web::EssView &out);            // main.cpp: the ESS settings and what they do
bool listener_inverter_command(const char *key, const char *value, char *why, size_t cap);
void listener_charger(charger::View &out);
bool listener_charger_read(char *why, size_t cap);
bool listener_charger_set(const charger::Change &change, char *why, size_t cap);
void listener_assist(charger::View &out);
bool listener_assist_read(char *why, size_t cap);
bool listener_assist_set(const charger::Change &change, char *why, size_t cap);
void listener_system(uint32_t &boot_id, bool &pending, const char *&message);
bool listener_restart(char *why, size_t cap);
bool listener_wifi_restart();
bool listener_firmware_allowed();
const char *listener_firmware_restart(firmware_ota::Action action);
size_t listener_ac_input_json(char *out,size_t cap);
bool listener_ac_input_request(bool ignore,uint32_t base,char *why,size_t cap);
bool listener_ac_input_config(const acinput::Config &c,uint32_t base,char *why,size_t cap);
size_t listener_pv_json(char *out,size_t cap);
bool listener_pv_request(bool disable,uint32_t base,char *why,size_t cap);
bool listener_pv_config(const pvdisable::Config &c,uint32_t base,char *why,size_t cap);
const char *listener_time_zone(bool &manual);
bool listener_time_zone_set(const controller_time::Zone *zone, bool manual, bool first_detection);
void listener_mqtt(ha::Settings &out);           // main.cpp: the MQTT settings
bool listener_settings_read(bool inspection);
void listener_settings(vebus::Requester::SettingsRead &out, web::InverterSetupView *setup);
// main.cpp: writing a settings file to the Multi (upload.h)
bool listener_upload_load(const uint8_t *data, size_t n, const char *name, char *why, size_t cap);
bool listener_upload_start(char *why, size_t cap, bool access_only);
bool listener_upload_cancel(char *why, size_t cap);
void listener_upload_clear();
void listener_upload(web::UploadView &v);
bool listener_mqtt_set(const ha::Settings &s, char *why, size_t cap);  // main.cpp: check, save, reconnect

namespace web {

namespace {

constexpr const char *HOSTNAME = "brain-zero";
constexpr const char *LEGACY_HOSTNAME = "vebus-listener";
constexpr uint32_t MDNS_RETRY_MS = 5000;
WebServer server(80);
Preferences ui_prefs;
device_settings::Network network, candidate;
device_settings::NetworkTrial network_trial;
String language = "en";
const char *network_notice = "";
bool enabled = false, mdns_up = false;
IPAddress served_ip, mdns_alias_ip;
uint32_t served_epoch = 0, mdns_at = 0, cycle_at = 0, wifi_cycles = 0;
enum class Cycle { IDLE, DISCONNECTING, RADIO_OFF };
Cycle cycle = Cycle::IDLE;
// The restart guard survives ESP.restart() and clears after a stable connection.
// Keep the last responsive gateway separately, including across normal OTA reboots.
RTC_NOINIT_ATTR uint32_t wifi_restart_cookie;
RTC_NOINIT_ATTR uint32_t wifi_verified_gateway;
RTC_NOINIT_ATTR uint32_t wifi_gateway_cookie;
constexpr uint32_t WIFI_RESTART_USED = 0xf34a98b2;
constexpr uint32_t WIFI_GATEWAY_VERIFIED = 0x6bca2017;
bool restart_shutdown = false;
std::atomic<uint32_t> wifi_epoch{0};
std::atomic<unsigned> disconnect_reason{0};
std::atomic<unsigned> disconnect_count{0};
wifi_recovery::Policy wifi_policy;
esp_ping_handle_t gateway_ping = nullptr;
uint32_t probe_gateway = 0, probe_at = 0, probe_epoch = 0;
bool probe_pending = false;
std::atomic<int> probe_result{0};
EssView view;  // handlers run on the Arduino loop task only

void start_station() {
    WiFi.setHostname(HOSTNAME);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(true);
#ifdef WIFI_TX_POWER_QDBM
    const esp_err_t result = esp_wifi_set_max_tx_power(WIFI_TX_POWER_QDBM);
    if (result != ESP_OK) Serial0.printf("# wifi: setting transmit limit failed (%s)\n", esp_err_to_name(result));
#endif
#ifdef WIFI_CHANNEL_WIDTH_20
    const esp_err_t width_result = esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20);
    if (width_result != ESP_OK) Serial0.printf("# wifi: setting channel width failed (%s)\n", esp_err_to_name(width_result));
#endif
#ifdef WIFI_LEGACY_BG
    const esp_err_t protocol_result = esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G);
    if (protocol_result != ESP_OK) Serial0.printf("# wifi: setting legacy protocol failed (%s)\n", esp_err_to_name(protocol_result));
#endif
    const auto &join = network_trial.testing() ? candidate : network;
    WiFi.begin(join.ssid, join.password);
}

void refresh_mdns(const IPAddress &ip, uint32_t now) {
    if ((mdns_up && ip == mdns_alias_ip) || now - mdns_at < MDNS_RETRY_MS) return;
    mdns_at = now;
    if (!mdns_up) {
        if (!MDNS.begin(HOSTNAME)) return;
        mdns_up = true;
        MDNS.addService("http", "tcp", 80);
        // Android devices without .local DNS can use DNS-SD to find the default address.
        if (mdns_service_add(HOSTNAME, "_brain-zero", "_tcp", 80, nullptr, 0) != ESP_OK) {
            Serial0.println("# wifi: mobile discovery service unavailable");
        }
    }

    if (uint32_t(mdns_alias_ip)) {
        // Both operations run in order on the mDNS task. Remove the old address before adding the new one.
        if (mdns_delegate_hostname_remove(LEGACY_HOSTNAME) != ESP_OK) return;
        mdns_alias_ip = IPAddress();
    }
    mdns_ip_addr_t legacy = {};
    legacy.addr.type = ESP_IPADDR_TYPE_V4;
    legacy.addr.u_addr.ip4.addr = uint32_t(ip);
    if (mdns_delegate_hostname_add(LEGACY_HOSTNAME, &legacy) == ESP_OK) {
        mdns_alias_ip = ip;
    } else {
        Serial0.println("# wifi: old hostname alias unavailable");
    }
}

void stop_http() {
    // ESP-IDF tracks station link changes itself. Keep mDNS alive because the
    // bundled SDK leaves a dangling delegated hostname list after mdns_free().
    if (uint32_t(served_ip)) server.stop();
    served_ip = IPAddress();
}

void pause_station(uint32_t now) {
    cycle = Cycle::DISCONNECTING;
    cycle_at = now;
    ++wifi_epoch;
    stop_http();
    WiFi.setAutoReconnect(false);
    WiFi.disconnect();
}

void cycle_station(uint32_t now, const char *reason) {
    if (cycle != Cycle::IDLE) return;
    ++wifi_cycles;
    wifi_policy.cycled(now);
    Serial0.printf("# wifi: rejoining (%s), cycles=%lu, last_disconnect=%u, ap=%s, channel=%d\n",
                   reason, (unsigned long)wifi_cycles, disconnect_reason.load(),
                   WiFi.BSSIDstr().c_str(), WiFi.channel());
    pause_station(now);
}

// The SDK runs each one-packet ICMP check in its own task. The loop does not
// wait for replies, and a reply from a previous network epoch is ignored.
void probe_gateway_path(uint32_t now, uint32_t gateway, uint32_t epoch) {
    wifi_policy.observe_gateway(gateway);
    if (probe_pending) {
        const int result = probe_result.exchange(0);
        if (!result) return;
        probe_pending = false;
        if (probe_gateway == gateway && probe_epoch == epoch) {
            wifi_policy.probe(result > 0);
            if (result > 0) {
                wifi_verified_gateway = gateway;
                wifi_gateway_cookie = WIFI_GATEWAY_VERIFIED;
            }
        }
    }
    if (!gateway || (gateway == probe_gateway && now - probe_at < wifi_recovery::PROBE_MS)) return;
    probe_at = now;
    if (gateway != probe_gateway && gateway_ping) {
        esp_ping_delete_session(gateway_ping);
        gateway_ping = nullptr;
    }
    probe_gateway = gateway;
    if (!gateway_ping) {
        esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();
        config.count = 1;
        config.data_size = 16;
        ip_addr_set_ip4_u32(&config.target_addr, gateway);
        esp_ping_callbacks_t callbacks = {};
        callbacks.on_ping_end = [](esp_ping_handle_t session, void *) {
            uint32_t replies = 0;
            esp_ping_get_profile(session, ESP_PING_PROF_REPLY, &replies, sizeof replies);
            probe_result.store(replies ? 1 : -1);
        };
        if (esp_ping_new_session(&config, &callbacks, &gateway_ping) != ESP_OK) return;
    }
    probe_epoch = epoch;
    probe_pending = esp_ping_start(gateway_ping) == ESP_OK;
}

// Appends to a fixed buffer, keeping it terminated.
struct Out {
    char *p;
    size_t cap, n = 0;
    bool cut = false;  // something did not fit
    Out(char *buf, size_t size) : p(buf), cap(size) { p[0] = 0; }
    void add(const char *fmt, ...) {
        if (n + 1 >= cap) {
            cut = true;
            return;
        }
        va_list a;
        va_start(a, fmt);
        int k = vsnprintf(p + n, cap - n, fmt, a);
        va_end(a);
        if (k > 0 && (size_t)k >= cap - n) cut = true;
        if (k > 0) n += (size_t)k < cap - n ? (size_t)k : cap - n - 1;
    }
    void str(const char *s) {  // a JSON string
        add("\"");
        for (; *s; s++) {
            if (*s == '"' || *s == '\\') {
                add("\\%c", *s);
            } else if ((unsigned char)*s >= 0x20) {
                add("%c", *s);
            }
        }
        add("\"");
    }
};

const char *tf(bool b) { return b ? "true" : "false"; }

// What the ESS settings make this ESP32 do now.
void status_json(const EssView &v, Out &o) {
    const ess::Decision &d = v.decision;
    char voltage[16] = "null";
    if (v.voltage_valid) snprintf(voltage, sizeof voltage, "%.2f", v.voltage);
    char soc[16];
    if (v.soc_valid) {
        snprintf(soc, sizeof soc, "%.1f", v.soc);
    } else {
        snprintf(soc, sizeof soc, "null");
    }
    o.add("{\"run\":%s,\"ctl\":\"%s\",\"mode\":%d,\"mode_name\":\"%s\",\"state\":%u,\"state_name\":\"%s\",\"why\":\"%s\","
          "\"setpoint_w\":%d,\"target_w\":%d,\"flags\":%u,\"multi_flags\":%d,\"schedule\":%d,\"charging\":%s,"
          "\"capped\":%s,\"active_soc_limit\":%.1f,\"soc_limit\":%.1f,\"reached\":%u,\"soc\":%s,\"clock\":\"%s\","
          "\"clock_valid\":%s,\"battery_voltage\":%s,\"voltage_hold\":%s,\"voltage_hold_confirmed\":%s,"
          "\"charge_inhibited\":%s,\"grid_hold\":%s,\"detect_time_zone\":%s,\"time_zone\":",
          tf(v.settings.enabled), v.ctl, (int)v.settings.mode, ess::mode_name(v.settings.mode), d.state,
          ess::state_name(d.state), d.why, d.setpoint_w, d.target_w, d.flags, v.multi_flags, d.schedule, tf(d.charging),
          tf(d.capped), d.active_soc_limit, v.memory.soc_limit, v.memory.flags, soc, v.clock, tf(v.clock_valid),
          voltage, tf(d.voltage_hold), tf(v.voltage_hold_confirmed), tf(d.charge_inhibited), tf(d.grid_hold),
          tf(v.detect_time_zone));
    o.str(v.time_zone); o.add("}");
}

// The settings, named as the form fields.
void settings_json(const ess::Settings &s, Out &o) {
    o.add("{\"run\":%d,\"mode\":%d,\"min_soc\":%.1f,\"grid_setpoint\":%d,\"max_charge\":%d,\"max_discharge\":%d,"
          "\"max_feed_in\":%d,\"max_import\":%d,\"always_peak_shave\":%d,\"dc_pv_feed_in\":%d,\"ext_no_charge\":%d,"
          "\"ext_no_feed_in\":%d,\"safety_max\":%d,\"voltage_hold_cv\":%u,\"tz\":\"%s\",\"schedules\":[",
          s.enabled, (int)s.mode, s.min_soc, s.grid_setpoint_w, s.max_charge_w, s.max_discharge_w, s.max_feed_in_w,
          s.max_import_w, s.always_peak_shave, s.dc_pv_feed_in, s.ext_no_charge, s.ext_no_feed_in, s.safety_max_w,
          unsigned(s.voltage_hold_cv), s.tz);
    for (int i = 0; i < ess::SCHEDULES; i++) {
        const ess::Schedule &w = s.schedule[i];
        o.add("%s{\"on\":%d,\"day\":%u,\"start\":%ld,\"duration\":%ld,\"soc\":%u,\"discharge\":%d}", i ? "," : "",
              w.enabled, w.day, (long)w.start_s, (long)w.duration_s, w.soc, w.allow_discharge);
    }
    // Keep the original six-field rows for existing Home Assistant clients.
    o.add("],\"schedule_grid_after_target\":[");
    for (int i = 0; i < ess::SCHEDULES; i++) o.add("%s%d", i ? "," : "", s.grid_after_target(i));
    o.add("]}");
}

// ---------------------------------------------------------------- reading the form

// Form fields into settings; a field left out keeps its value. The first bad one is named in `bad`.
// HTTP form requests and MQTT commands share the validation and readback handlers.
// The web handlers and MQTT dispatcher run sequentially on the Arduino loop task.
// Responses use one buffer, including the binary VEConfigure transfer.
constexpr size_t CONTROL_BODY_BYTES = rvsc::MAX_SIZE, MQTT_HEADER_BYTES = 32;
char control_response[MQTT_HEADER_BYTES + CONTROL_BODY_BYTES + 2];
char *const control_body = control_response + MQTT_HEADER_BYTES;
char json_fragment[2048];

class ControlExchange {
  public:
    explicit ControlExchange(WebServer &http) : http_(&http) {}
    ControlExchange(const cJSON *fields, char *reply, size_t capacity)
        : fields_(fields), reply_(reply), capacity_(capacity) {}

    int status = 500;
    int args() const { return http_ ? http_->args() : cJSON_GetArraySize(fields_); }
    bool hasArg(const char *key) const {
        return http_ ? http_->hasArg(key) : cJSON_GetObjectItemCaseSensitive(fields_, key) != nullptr;
    }
    String arg(const char *key) const {
        return http_ ? http_->arg(key) : value(cJSON_GetObjectItemCaseSensitive(fields_, key));
    }
    String arg(int index) const { return http_ ? http_->arg(index) : value(cJSON_GetArrayItem(fields_, index)); }
    String argName(int index) const {
        if (http_) return http_->argName(index);
        const auto *field = cJSON_GetArrayItem(fields_, index);
        return field && field->string ? String(field->string) : String();
    }
    void sendHeader(const char *key, const char *value) {
        if (http_) http_->sendHeader(key, value);
    }
    void send(int code, const char *content_type, const char *body) {
        status = code;
        if (http_) {
            if (body == control_body) http_->send_P(code, content_type, body);
            else http_->send(code, content_type, body);
        } else if (body == reply_) {
            return;
        } else if (strlen(body) < capacity_) {
            memcpy(reply_, body, strlen(body) + 1);
        } else {
            status = 500;
            snprintf(reply_, capacity_, "{\"error\":\"Controller response exceeded the size limit\"}");
        }
    }

  private:
    static String value(const cJSON *item) {
        return cJSON_IsString(item) ? String(item->valuestring) : String();
    }
    WebServer *http_ = nullptr;
    const cJSON *fields_ = nullptr;
    char *reply_ = nullptr;
    size_t capacity_ = 0;
};

class Fields {
    ControlExchange &input;
  public:
    explicit Fields(ControlExchange &request) : input(request) {}
    char bad[48] = "";
    bool get_long(const char *name, long *v, long lo, long hi) {
        if (!input.hasArg(name)) return false;
        String a = input.arg(name);
        char *end = nullptr;
        long x = strtol(a.c_str(), &end, 10);
        if (a.length() == 0 || *end != 0 || x < lo || x > hi) return fail(name);
        *v = x;
        return true;
    }
    void get(const char *name, int *v, long lo, long hi) {
        long x;
        if (get_long(name, &x, lo, hi)) *v = (int)x;
    }
    void get(const char *name, bool *v) {
        long x;
        if (get_long(name, &x, 0, 1)) *v = x != 0;
    }
    // Text, trimmed; whether it fits is the caller's check (ha::validate and the like).
    void get_text(const char *name, char *dst, size_t cap) {
        if (!input.hasArg(name)) return;
        String a = input.arg(name);
        a.trim();
        if (a.length() >= cap) {
            fail(name);
            return;
        }
        snprintf(dst, cap, "%s", a.c_str());
    }
    void get(const char *name, float *v, float lo, float hi) {
        if (!input.hasArg(name)) return;
        String a = input.arg(name);
        char *end = nullptr;
        float x = strtof(a.c_str(), &end);
        if (a.length() == 0 || *end != 0 || !(x >= lo && x <= hi)) {
            fail(name);
            return;
        }
        *v = x;
    }
    // "h:mm", "h:mm:ss" or seconds
    void get_time(const char *name, int32_t *v, long hi) {
        if (!input.hasArg(name)) return;
        String a = input.arg(name);
        const char *t = a.c_str();
        long h = -1, m = 0, sec = 0, secs = -1;
        char more;
        if (sscanf(t, "%ld:%ld:%ld%c", &h, &m, &sec, &more) == 3 || sscanf(t, "%ld:%ld%c", &h, &m, &more) == 2) {
            if (h >= 0 && m >= 0 && m <= 59 && sec >= 0 && sec <= 59) secs = h * 3600 + m * 60 + sec;
        } else if (sscanf(t, "%ld%c", &h, &more) == 1) {
            secs = h;
        }
        if (secs < 0 || secs > hi) {
            fail(name);
            return;
        }
        *v = (int32_t)secs;
    }

  private:
    bool fail(const char *name) {
        if (!bad[0]) snprintf(bad, sizeof bad, "%s", name);
        return false;
    }
};

bool settings_from_form(ControlExchange &input, ess::Settings &s, char *why, size_t cap) {
    Fields f(input);
    f.get("run", &s.enabled);
    if (input.hasArg("stop")) s.enabled = false;
    long mode;
    if (f.get_long("mode", &mode, 0, 3)) s.mode = (ess::Mode)mode;
    f.get("min_soc", &s.min_soc, 0, 100);
    f.get("grid_setpoint", &s.grid_setpoint_w, -ess::HARD_MAX_W, ess::HARD_MAX_W);
    f.get("max_charge", &s.max_charge_w, -1, ess::HARD_MAX_W);
    f.get("max_discharge", &s.max_discharge_w, -1, ess::HARD_MAX_W);
    f.get("max_feed_in", &s.max_feed_in_w, -1, ess::HARD_MAX_W);
    f.get("max_import", &s.max_import_w, -1, ess::HARD_MAX_W);
    f.get("always_peak_shave", &s.always_peak_shave);
    f.get("dc_pv_feed_in", &s.dc_pv_feed_in);
    f.get("ext_no_charge", &s.ext_no_charge);
    f.get("ext_no_feed_in", &s.ext_no_feed_in);
    f.get("safety_max", &s.safety_max_w, 0, ess::HARD_MAX_W);
    long voltage_hold;
    if (f.get_long("voltage_hold_cv", &voltage_hold, 0, ess::MAX_HOLD_CV)) s.voltage_hold_cv = uint16_t(voltage_hold);
    if (input.hasArg("tz")) {
        String tz = input.arg("tz");
        tz.trim();
        if (tz.length() == 0 || tz.length() >= sizeof s.tz || tz.indexOf('"') >= 0 || tz.indexOf('\\') >= 0) {
            snprintf(f.bad, sizeof f.bad, "tz");
        } else {
            snprintf(s.tz, sizeof s.tz, "%s", tz.c_str());
        }
    }
    for (int i = 0; i < ess::SCHEDULES; i++) {
        ess::Schedule &w = s.schedule[i];
        char name[sizeof "s00_grid_after_target"];
        long day;
        snprintf(name, sizeof name, "s%d_on", i + 1);
        f.get(name, &w.enabled);
        snprintf(name, sizeof name, "s%d_day", i + 1);
        if (f.get_long(name, &day, 0, ess::DAY_MONTHLY)) w.day = (uint8_t)day;
        snprintf(name, sizeof name, "s%d_start", i + 1);
        f.get_time(name, &w.start_s, 86399);
        snprintf(name, sizeof name, "s%d_duration", i + 1);
        f.get_time(name, &w.duration_s, 86400);
        int soc = w.soc;
        snprintf(name, sizeof name, "s%d_soc", i + 1);
        f.get(name, &soc, 0, 100);
        w.soc = (uint8_t)soc;
        snprintf(name, sizeof name, "s%d_discharge", i + 1);
        f.get(name, &w.allow_discharge);
        bool grid = s.grid_after_target(i);
        snprintf(name, sizeof name, "s%d_grid_after_target", i + 1);
        f.get(name, &grid);
        s.grid_after_target(i, grid);
    }
    if (f.bad[0]) {
        snprintf(why, cap, "bad value for %s", f.bad);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- handlers

void handle_root() { server.send_P(200, "text/html", DASHBOARD_HTML); }

void handle_ess_page() { server.send_P(200, "text/html", ESS_HTML); }

bool decimal_u32(const char *s, uint32_t &v) {
    if (!s || !*s) return false;
    uint32_t n = 0;
    for (; *s; ++s) {
        if (*s < '0' || *s > '9' || n > (UINT32_MAX - (*s - '0')) / 10) return false;
        n = n * 10 + (*s - '0');
    }
    v = n;
    return true;
}

void handle_inverter_page() { server.send_P(200, "text/html", INVERTER_HTML); }

void handle_inverter_get(ControlExchange &io) {
    listener_ess(view);
    size_t n = inverter::to_json(view.inverter, control_body, CONTROL_BODY_BYTES);
    if (!n || control_body[n - 1] != '}') {
        io.send(500, "application/json", "{\"error\":\"Inverter status overflow\"}");
        return;
    }
    Out o(control_body + n - 1, CONTROL_BODY_BYTES - n + 1);
    listener_ac_input_json(json_fragment, sizeof json_fragment);
    o.add(",\"ac_input\":%s", json_fragment);
    listener_pv_json(json_fragment, sizeof json_fragment);
    o.add(",\"pv_disable\":%s}", json_fragment);
    io.sendHeader("Cache-Control", "no-store");
    io.send(o.cut ? 500 : 200, "application/json", o.cut ? "{\"error\":\"Inverter status overflow\"}" : control_body);
}

void handle_ac_input_get() {
    listener_ac_input_json(control_body, CONTROL_BODY_BYTES);
    server.sendHeader("Cache-Control", "no-store");
    server.send_P(200, "application/json", control_body);
}
bool ac_number(ControlExchange &io, const char *key,float &v){
    String s=io.arg(key);if(!s.length())return false;char *end=nullptr;v=strtof(s.c_str(),&end);return end&&!*end&&isfinite(v);
}
void handle_ac_input_post(ControlExchange &io, bool settings){
    char why[180]="Send only the supported AC-input fields",json[256];uint32_t base=0,value=0;
    bool ok=io.hasArg("base")&&decimal_u32(io.arg("base").c_str(),base);
    if(settings){acinput::Config c;uint32_t minutes=0;
        ok=ok&&io.args()==4&&io.hasArg("reconnect_soc")&&io.hasArg("reconnect_v")&&io.hasArg("minutes")&&
            ac_number(io, "reconnect_soc",c.reconnect_soc)&&ac_number(io, "reconnect_v",c.reconnect_v)&&
            decimal_u32(io.arg("minutes").c_str(),minutes)&&minutes>=1&&minutes<=240;
        if(ok){c.minutes=minutes;ok=listener_ac_input_config(c,base,why,sizeof why);}
    }else{ok=ok&&io.args()==2&&io.hasArg("ignore")&&decimal_u32(io.arg("ignore").c_str(),value)&&value<=1;
        if(ok)ok=listener_ac_input_request(value,base,why,sizeof why);}
    Out o(json,sizeof json);o.add("{\"ok\":%s,\"%s\":",tf(ok),ok?"message":"error");o.str(ok?(settings?"Protection settings saved":"Requested; actual inverter readback follows"):why);o.add("}");
    io.send(ok?202:409,"application/json",json);
}
void handle_pv_get() {
    listener_pv_json(control_body, CONTROL_BODY_BYTES);
    server.sendHeader("Cache-Control", "no-store");
    server.send_P(200, "application/json", control_body);
}
void handle_pv_post(ControlExchange &io, bool settings){
    char why[160]="Send only base and the supported PV control field",json[256];uint32_t base=0,value=0;
    const char *key=settings?"seconds":"disable";
    bool ok=io.args()==2 && io.hasArg("base") && io.hasArg(key) &&
        decimal_u32(io.arg("base").c_str(),base) && decimal_u32(io.arg(key).c_str(),value);
    if(ok && settings){pvdisable::Config c;ok=value>=10 && value<=300;if(ok){c.seconds=value;ok=listener_pv_config(c,base,why,sizeof why);}}
    else if(ok){ok=value<=1;if(ok)ok=listener_pv_request(value,base,why,sizeof why);}
    Out o(json,sizeof json);o.add("{\"ok\":%s,\"%s\":",tf(ok),ok?"message":"error");o.str(ok?(settings?"Restoration duration saved":"Requested; wait for independent inverter flag readback"):why);o.add("}");
    io.send(ok?202:409,"application/json",json);
}

void handle_inverter_post(ControlExchange &io) {
    char why[160] = "Send exactly one inverter control", json[256];
    bool ok = io.args() == 1 && listener_inverter_command(io.argName(0).c_str(), io.arg(0).c_str(), why, sizeof why);
    Out o(json, sizeof json);
    o.add("{\"ok\":%s,\"%s\":", tf(ok), ok ? "message" : "error");
    o.str(ok ? "Queued; confirmation will follow from the inverter" : why);
    o.add("}");
    io.send(ok ? 202 : 409, "application/json", json);
}

void handle_charger_get(ControlExchange &io) {
    charger::View v;
    listener_charger(v);
    size_t n = charger::to_json(v, control_body, CONTROL_BODY_BYTES);
    io.sendHeader("Cache-Control", "no-store");
    io.send(n ? 200 : 500, "application/json", n ? control_body : "{\"error\":\"Charger status buffer is full\"}");
}

void handle_charger_post(ControlExchange &io) {
    char why[160] = "Send charge_current, absorption_voltage, float_voltage and base", json[256];
    charger::Change change;
    bool ok = io.args() == 4 && io.hasArg("base") && io.hasArg("charge_current") &&
              io.hasArg("absorption_voltage") && io.hasArg("float_voltage") &&
              charger::parse(io.arg("charge_current").c_str(), io.arg("absorption_voltage").c_str(),
                             io.arg("float_voltage").c_str(), io.arg("base").c_str(), change, why, sizeof why) &&
              listener_charger_set(change, why, sizeof why);
    Out o(json, sizeof json);
    o.add("{\"ok\":%s,\"%s\":", tf(ok), ok ? "message" : "error");
    o.str(ok ? "Reading current settings before saving; wait for inverter readback" : why);
    o.add("}");
    io.send(ok ? 202 : 409, "application/json", json);
}

void handle_charger_read(ControlExchange &io) {
    char why[160] = "", json[256];
    bool ok = listener_charger_read(why, sizeof why);
    Out o(json, sizeof json);
    o.add("{\"ok\":%s,\"%s\":", tf(ok), ok ? "message" : "error");
    o.str(ok ? "Reading charger settings from the inverter" : why);
    o.add("}");
    io.send(ok ? 202 : 409, "application/json", json);
}

void handle_assist_get(ControlExchange &io) {
    charger::View v;
    listener_assist(v);
    size_t n = charger::to_json(v, control_body, CONTROL_BODY_BYTES);
    io.sendHeader("Cache-Control", "no-store");
    io.send(n ? 200 : 500, "application/json", n ? control_body : "{\"error\":\"Settings response overflow\"}");
}
void handle_assist_post(ControlExchange &io) {
    char why[160] = "Send power_assist, boost_factor and base", json[256];
    charger::Change change;
    bool ok = io.args() == 3 && io.hasArg("power_assist") && io.hasArg("boost_factor") && io.hasArg("base") &&
        charger::parse_assist(io.arg("power_assist").c_str(), io.arg("boost_factor").c_str(), io.arg("base").c_str(), change, why, sizeof why) &&
        listener_assist_set(change, why, sizeof why);
    Out o(json, sizeof json); o.add("{\"ok\":%s,\"%s\":", tf(ok), ok ? "message" : "error");
    o.str(ok ? "Reading settings before saving; wait for inverter readback" : why); o.add("}");
    io.send(ok ? 202 : 409, "application/json", json);
}
void handle_assist_read(ControlExchange &io) {
    char why[160] = "", json[256];
    bool ok = io.args() == 0 && listener_assist_read(why, sizeof why);
    Out o(json, sizeof json); o.add("{\"ok\":%s,\"%s\":", tf(ok), ok ? "message" : "error");
    o.str(ok ? "Reading PowerAssist settings" : why); o.add("}");
    io.send(ok ? 202 : 409, "application/json", json);
}

void handle_system_get(ControlExchange &io) {
    uint32_t boot_id; bool pending; const char *message;
    listener_system(boot_id, pending, message);
    String mac = WiFi.macAddress(); mac.replace(":", ""); mac.toLowerCase();
    char json[300]; Out o(json, sizeof json);
    o.add("{\"controller\":"); o.str(mac.c_str());
    o.add(",\"boot_id\":%u,\"restart_pending\":%s,\"uptime_s\":%.1f,\"message\":", boot_id, tf(pending), esp_timer_get_time() / 1e6);
    o.str(message); o.add("}");
    io.sendHeader("Cache-Control", "no-store"); io.send(200, "application/json", json);
}
void handle_system_post(ControlExchange &io) {
    char why[160] = "Send reboot=PRESS", json[256];
    bool ok = io.args() == 1 && io.hasArg("reboot") && io.arg("reboot") == "PRESS" && listener_restart(why, sizeof why);
    Out o(json, sizeof json); o.add("{\"ok\":%s,\"%s\":", tf(ok), ok ? "message" : "error");
    o.str(ok ? "Restart queued; wait for a different boot identifier" : why); o.add("}");
    io.send(ok ? 202 : 409, "application/json", json);
}

constexpr char OTA_TOKEN[] = "X-Brain-Zero-OTA", OTA_SIZE[] = "X-Firmware-Size";
constexpr char OTA_BUSY[] = "A GitHub firmware operation is already in progress.";
struct FirmwareUpload {
    bool seen = false, owns = false, complete = false;
    const char *error = nullptr;
} firmware_upload;

String request_host() {
    String host = server.hostHeader();
    if (host.endsWith(":80")) host.remove(host.length() - 3);
    return host;
}

bool firmware_host() { return local_host(request_host()); }

void console_error(int code, const char *error) {
    char json[240];
    Out o(json, sizeof json);
    o.add("{\"ok\":false,\"error\":"); o.str(error); o.add("}");
    server.sendHeader("Cache-Control", "no-store");
    server.send(code, "application/json", json);
}
void console_ok() {
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", "{\"ok\":true}");
}
void redirect(const char *path) {
    server.sendHeader("Location", path);
    server.sendHeader("Cache-Control", "no-store");
    server.send(303, "text/plain", "");
}
bool same_origin() {
    const String origin = server.header("Origin"), host = request_host();
    return server.header("Sec-Fetch-Site") != "cross-site" &&
        (!origin.length() || (local_host(host) && origin.equalsIgnoreCase("http://" + host)));
}
bool console_allowed(bool machine, bool report = true) {
    const bool api = server.uri().startsWith("/api/") || server.uri() == "/mcp";
    const bool host = firmware_host();
    if ((server.method() != HTTP_GET && !same_origin()) || ((!machine || console_auth::enabled()) && !host)) {
        if (report) console_error(403, "Open the controller using its local address.");
        return false;
    }
    if (!console_auth::enabled()) return true;
    const auto access = console_auth::access(server.header("Cookie"), server.header("Authorization"), millis());
    if (access == console_auth::Access::SESSION || (machine && api && access == console_auth::Access::API)) return true;
    if (report) {
        if (api) console_error(401, "Sign in to the Remote Console.");
        else redirect("/login");
    }
    return false;
}
void private_route(const char *path, HTTPMethod method, WebServer::THandlerFunction handler,
                   WebServer::THandlerFunction upload = {}) {
    const bool machine = strncmp(path, "/api/device", 11) && strncmp(path, "/api/firmware", 13) &&
                         strncmp(path, "/api/upload", 11);
    auto main = [handler, machine] { if (console_allowed(machine)) handler(); };
    if (upload) server.on(path, method, main, [upload, machine] { if (console_allowed(machine, false)) upload(); });
    else server.on(path, method, main);
}
void handle_login() {
    if (!firmware_host() || !same_origin()) { console_error(403, "Open the controller using its local address."); return; }
    if (console_auth::enabled()) {
        const uint32_t now = millis();
        if (console_auth::throttled(now)) { console_error(429, "Try again shortly."); return; }
        if (!console_auth::check_password(server.arg("password"), now)) {
            console_error(401, "Incorrect password"); return;
        }
        server.sendHeader("Set-Cookie", console_auth::sign_in(millis()));
    }
    console_ok();
}
void handle_logout() {
    if (!same_origin()) { console_error(403, "Open the controller using its local address."); return; }
    console_auth::sign_out(server.header("Cookie"), millis());
    server.sendHeader("Set-Cookie", "bz_session=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
    console_ok();
}
bool admin_password() {
    if (!firmware_host()) { console_error(403, "Open the controller using its local address."); return false; }
    if (console_auth::throttled(millis())) { console_error(429, "Try again shortly."); return false; }
    if (console_auth::check_password(server.arg("current"), millis())) return true;
    console_error(403, "Incorrect password");
    return false;
}
void handle_console_password() {
    if (!admin_password()) return;
    if (server.arg("disable") == "1") {
        if (!console_auth::reset()) { console_error(500, "Could not save settings."); return; }
        server.sendHeader("Set-Cookie", "bz_session=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
    } else {
        const String password = server.arg("password");
        if (password.length() < 12 || password.length() > 64) {
            console_error(400, "Use a password of 12–64 bytes."); return;
        }
        if (!console_auth::set_password(password)) { console_error(500, "Could not save settings."); return; }
        server.sendHeader("Set-Cookie", console_auth::sign_in(millis()));
    }
    console_ok();
}
void handle_console_token() {
    if (!admin_password()) return;
    char token[65];
    if (!console_auth::rotate_token(token)) { console_error(500, "Could not save settings."); return; }
    char json[100];
    snprintf(json, sizeof json, "{\"ok\":true,\"token\":\"%s\"}", token);
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", json);
}
bool wifi_confirmable() {
    return network_trial.testing() && network_trial.seconds(millis()) && cycle == Cycle::IDLE &&
           WiFi.status() == WL_CONNECTED && uint32_t(WiFi.localIP()) && WiFi.SSID() == candidate.ssid;
}
void handle_device() {
    char json[640];
    Out o(json, sizeof json);
    const bool connected = WiFi.status() == WL_CONNECTED && uint32_t(WiFi.localIP());
    o.add("{\"ok\":true,\"language\":"); o.str(language.c_str());
    bool manual;
    const char *zone = listener_time_zone(manual);
    o.add(",\"time_zone_mode\":\"%s\",\"time_zone\":", manual ? "manual" : "auto"); o.str(zone);
    o.add(",\"password_enabled\":%s,\"token_set\":%s,\"wifi\":{\"connected\":%s,\"ssid\":",
          tf(console_auth::enabled()), tf(console_auth::has_token()), tf(connected));
    o.str(connected ? WiFi.SSID().c_str() : network.ssid);
    o.add(",\"ip\":"); o.str(connected ? WiFi.localIP().toString().c_str() : "");
    o.add(",\"rssi\":%d,\"testing\":%s,\"confirmable\":%s,\"seconds\":%u,\"notice\":",
          connected ? int(WiFi.RSSI()) : 0, tf(network_trial.pending()), tf(wifi_confirmable()),
          unsigned(network_trial.seconds(millis())));
    o.str(network_notice); o.add("}}");
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", json);
}
void handle_network_test() {
    if (!firmware_host()) { console_error(403, "Open the controller using its local address."); return; }
    if (network_trial.pending() || cycle != Cycle::IDLE || restart_shutdown) {
        console_error(409, "A connection test is already in progress."); return;
    }
    const String ssid = server.arg("ssid");
    String password = server.arg("password");
    if (!password.length() && ssid == network.ssid) password = network.password;
    device_settings::Network next;
    if (!ssid.length() || ssid.length() > 32 || password.length() > 64 ||
        strlen(ssid.c_str()) != ssid.length() || strlen(password.c_str()) != password.length()) {
        console_error(400, "Enter a valid Wi-Fi network and password."); return;
    }
    memcpy(next.ssid, ssid.c_str(), ssid.length());
    memcpy(next.password, password.c_str(), password.length());
    if (!device_settings::valid(next)) { console_error(400, "Enter a valid Wi-Fi network and password."); return; }
    if (!strcmp(next.ssid, network.ssid) && !strcmp(next.password, network.password)) {
        network_notice = "Already using this network.";
        console_ok();
        return;
    }
    candidate = next;
    network_notice = "";
    network_trial.schedule(millis());
    console_ok();
}
void handle_network_keep() {
    if (!firmware_host() || !wifi_confirmable()) { console_error(409, "Connect to the new network before keeping it."); return; }
    if (ui_prefs.putBytes("wifi", &candidate, sizeof candidate) != sizeof candidate) {
        console_error(500, "Could not save settings."); return;
    }
    network = candidate;
    candidate = {};
    network_trial.confirm();
    network_notice = "Wi-Fi settings saved.";
    console_ok();
}
void handle_network_revert() {
    if (!firmware_host() || !network_trial.pending()) { console_error(409, "There is no connection test."); return; }
    network_trial.cancel(millis());
    console_ok();
}
void handle_language() {
    const String next = server.arg("code");
    if (!ui_language(next.c_str())) { console_error(400, "Choose a supported language."); return; }
    if (next != language && ui_prefs.putString("language", next) != next.length()) {
        console_error(500, "Could not save settings."); return;
    }
    language = next;
    console_ok();
}

void handle_time_zone() {
    const String mode = server.arg("mode");
    const String id = server.arg("zone");
    const bool first_detection = !server.hasArg("mode");
    const bool manual = mode == "manual";
    const auto *zone = id.length() ? controller_time::find(id.c_str()) : nullptr;
    if ((!first_detection && !manual && mode != "auto") || ((first_detection || manual || id.length()) && !zone)) {
        console_error(400, "Choose a supported time zone."); return;
    }
    if (!listener_time_zone_set(zone, manual, first_detection)) {
        console_error(500, "Could not save settings."); return;
    }
    console_ok();
}

const char *firmware_access() {
    if (!firmware_host()) return "Open the firmware page using the controller's local address.";
    if (!firmware_ota::authorized(server.header(OTA_TOKEN).c_str())) return "Reload the firmware page and try again.";
    return nullptr;
}

void firmware_reply(int code, const char *why = nullptr) {
    char json[768];
    Out o(json, sizeof json);
    o.add("{\"ok\":%s", tf(!why));
    if (why) {
        o.add(",\"error\":");
        o.str(why);
    } else {
        const auto s = firmware_release_job::ota_status();
        uint32_t boot_id;
        bool pending;
        const char *ignored;
        listener_system(boot_id, pending, ignored);
        o.add(",\"boot_id\":%u,\"build\":", unsigned(boot_id));
        o.str(diagnostics::BUILD);
        o.add(",\"slot\":"); o.str(s.slot);
        o.add(",\"target\":"); o.str(s.target);
        o.add(",\"offset\":%u,\"max_bytes\":%u,\"staged_bytes\":%u,\"safe\":%s,"
              "\"staged\":%s,\"trial\":%s,\"pending\":%s,\"release_busy\":%s,\"token\":",
              s.running_offset, unsigned(s.max_bytes), unsigned(s.staged_bytes),
              tf(listener_firmware_allowed()), tf(s.staged), tf(s.trial), tf(pending), tf(firmware_release_job::busy()));
        o.str(s.token);
        o.add(",\"message\":"); o.str(s.message);
    }
    o.add("}");
    server.sendHeader("Cache-Control", "no-store");
    server.send(code, "application/json", json);
}

void firmware_release_reply(int code, const char *error = nullptr) {
    if (error) { console_error(code, error); return; }
    const auto s = firmware_release_job::status();
    char json[512];
    Out o(json, sizeof json);
    o.add("{\"ok\":true,\"status\":\"%s\",\"size\":%lu,\"received\":%u,\"preview\":%s,\"channel\":\"%s\",\"tag\":",
          s.phase, (unsigned long)s.build.size, unsigned(s.received), tf(s.build.preview),
          "public");
    o.str(s.build.tag);
    o.add(",\"can_stage\":%s,\"message\":", tf(s.can_stage)); o.str(s.message); o.add("}");
    server.sendHeader("Cache-Control", "no-store");
    server.send(code, "application/json", json);
}

void firmware_release_check() {
    const char *why = firmware_access();
    if (!why && firmware_release_job::busy()) why = OTA_BUSY;
    if (!why) {
        const auto s = firmware_ota::status();
        if (!s.max_bytes || s.trial || s.staged || s.queued != firmware_ota::Action::NONE)
            why = "Finish the current firmware update before checking GitHub.";
        else why = firmware_release_job::check(diagnostics::RELEASE_VERSION, s.max_bytes);
    }
    firmware_release_reply(why ? 409 : 202, why);
}

void firmware_release_stage() {
    const char *why = firmware_access();
    if (!why && firmware_release_job::busy()) why = OTA_BUSY;
    if (!why && !listener_firmware_allowed()) why = "Connect mains and wait for inverter operations to finish.";
    if (!why) why = firmware_release_job::stage(server.arg("tag").c_str());
    firmware_release_reply(why ? 409 : 202, why);
}

size_t firmware_size() {
    const String size = server.header(OTA_SIZE);
    if (!size.length() || size.length() > 7) return 0;
    size_t bytes = 0;
    for (size_t i = 0; i < size.length(); ++i) {
        if (size[i] < '0' || size[i] > '9') return 0;
        bytes = bytes * 10 + (size[i] - '0');
    }
    return bytes;
}

void firmware_upload_chunk() {
    const HTTPUpload &up = server.upload();
    auto &request = firmware_upload;
    if (up.status == UPLOAD_FILE_START) {
        if (request.seen) {
            request.error = "Upload one application file at a time.";
            return;
        }
        request.seen = true;
        request.error = firmware_access();
        if (request.error) return;
        if (firmware_release_job::busy()) { request.error = OTA_BUSY; return; }
        if (!listener_firmware_allowed()) {
            request.error = "Connect mains and wait for inverter operations to finish.";
            return;
        }
        String filename = up.filename;
        filename.toLowerCase();
        if (up.name != "firmware" || !filename.endsWith(".bin")) {
            request.error = "Choose a firmware.bin application file.";
            return;
        }
        request.error = firmware_ota::start(firmware_size());
        request.owns = !request.error;
    } else if (up.status == UPLOAD_FILE_WRITE && request.owns && !request.error) {
        request.error = firmware_ota::write(up.buf, up.currentSize);
    } else if (up.status == UPLOAD_FILE_END) {
        request.complete = true;
    }
}

void finish_firmware_upload() {
    auto &request = firmware_upload;
    const char *why = request.error;
    if (!why) why = request.owns && request.complete ? firmware_ota::finish() : "No complete application file received.";
    if (why && request.owns) firmware_ota::discard(why);
    request = {};
    firmware_reply(why ? 400 : 200, why);
}

void firmware_action(firmware_ota::Action action) {
    const char *why = firmware_access();
    if (!why && firmware_release_job::busy()) why = OTA_BUSY;
    if (!why) why = listener_firmware_restart(action);
    firmware_reply(why ? 409 : 202, why);
}

void send_state(ControlExchange &io, bool diagnostic) {
    static vebus::State s;  // handlers run on the Arduino loop task only
    char *out = control_body;
    faults::Report report;
    listener_state(s, &report);
    const int64_t now = esp_timer_get_time();
    const size_t n = vebus::to_json(s, now, nullptr, out, CONTROL_BODY_BYTES);
    if (!n || n >= CONTROL_BODY_BYTES || out[n - 1] != '}') {
        io.send(500, "application/json", "{\"error\":\"Controller snapshot overflow\"}");
        return;
    }
    listener_ess(view);
    Out e(out + n - 1, CONTROL_BODY_BYTES - n + 1);
    listener_tx_json(json_fragment, sizeof json_fragment);
    e.add(",\"rssi\":%d,\"heap\":%u,\"ip\":\"%s\",%s", (int)WiFi.RSSI(),
          (unsigned)ESP.getFreeHeap(), WiFi.localIP().toString().c_str(), json_fragment);
    mqtt_link::status_json(json_fragment, sizeof json_fragment);
    e.add(",\"mqtt\":%s,\"ess\":", json_fragment);
    status_json(view, e);
    wifi_ap_record_t ap = {};
    const bool associated = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    int8_t tx_power = 0;
    esp_wifi_get_max_tx_power(&tx_power);
    wifi_bandwidth_t width = WIFI_BW_HT20;
    const bool width_known = esp_wifi_get_bandwidth(WIFI_IF_STA, &width) == ESP_OK;
    e.add(",\"wifi_recovery\":{\"gateway_verified\":%s,\"gateway_misses\":%u,\"radio_cycles\":%u,"
          "\"disconnect_reason\":%u,\"disconnects\":%u,\"restart_used\":%s,"
          "\"bssid\":\"%s\",\"channel\":%u,\"ap_security\":%d,\"tx_power_qdbm\":%d,\"width_mhz\":%u}",
          tf(wifi_policy.confirmed()), wifi_policy.misses(), unsigned(wifi_cycles), disconnect_reason.load(),
          disconnect_count.load(), tf(wifi_restart_cookie == WIFI_RESTART_USED),
          associated ? WiFi.BSSIDstr().c_str() : "", unsigned(ap.primary), associated ? int(ap.authmode) : -1,
          int(tx_power), width_known ? (width == WIFI_BW_HT20 ? 20u : 40u) : 0u);
    listener_ac_input_json(json_fragment, sizeof json_fragment);
    e.add(",\"ac_input\":%s", json_fragment);
    listener_pv_json(json_fragment, sizeof json_fragment);
    e.add(",\"pv_disable\":%s", json_fragment);
    const float sustain_age = faults::age(report.extended_at, now);
    const bool sustain_known = report.firmware == faults::TESTED_FIRMWARE && report.valid_bits > 1 && sustain_age >= 0;
    // Native Venus mk2-dbus maps FF bit 1 to SustainMode. This is cached readback only.
    const auto &live = listener_warning_status();
    e.add(",\"warning_status\":{\"led_code\":%d,\"leds_known\":%s,\"levels\":[%d,%d,%d,%d],"
          "\"mains_lost\":%s,\"sustain\":%s,\"sustain_age_s\":%.1f,\"history_revision\":%u,\"extra_levels\":[",
          live.led_code, tf(live.leds_known), live.levels[0], live.levels[1],
          live.levels[2], live.levels[3],
          live.mains_lost < 0 ? "null" : tf(live.mains_lost != 0),
          sustain_known ? tf(report.extended_flags & 2u) : "null", sustain_known ? sustain_age : -1.f,
          unsigned(listener_warning_history().revision));
    for (int kind = warnings::VOLTAGE_SENSE; kind < warnings::KIND_COUNT; ++kind)
        e.add("%s%d", kind == warnings::VOLTAGE_SENSE ? "" : ",", live.levels[kind]);
    e.add("]}");
    if (diagnostic) {
        static diagnostics::Cache cached;
        listener_diagnostics(cached);
        uint32_t boot;
        bool pending;
        const char *message;
        listener_system(boot, pending, message);
        int reset = int(esp_reset_reason());
        multi_heap_info_t heap = {};
        heap_caps_get_info(&heap, MALLOC_CAP_INTERNAL);
        e.add(",\"diagnostics\":{\"schema\":1,\"build\":\"%s\",\"boot_id\":%u,\"reset_code\":%d,\"reset_reason\":\"%s\",\"heap_total\":%u,\"heap_used\":%u,\"min_heap\":%u,\"max_alloc_heap\":%u,\"loop_stack_min_free\":%u,\"restart_pending\":%s",
              diagnostics::BUILD, boot, reset, diagnostics::reset_name(reset),
              unsigned(heap.total_free_bytes + heap.total_allocated_bytes), unsigned(heap.total_allocated_bytes),
              unsigned(heap.minimum_free_bytes), unsigned(heap.largest_free_block),
              unsigned(uxTaskGetStackHighWaterMark(nullptr)), tf(pending));
        if (!diagnostics::to_json(cached, json_fragment, sizeof json_fragment)) e.cut = true;
        e.add(",\"cached\":%s", json_fragment);
        if (!charger::to_json(cached.charger, json_fragment, sizeof json_fragment)) e.cut = true;
        e.add(",\"charger\":%s", json_fragment);
        if (!faults::to_json(cached.faults, cached.now, cached.led_code, json_fragment, sizeof json_fragment)) e.cut = true;
        e.add(",\"faults\":%s}", json_fragment);
    }
    e.add("}");
    io.sendHeader("Cache-Control", "no-store");
    io.send(e.cut ? 500 : 200, "application/json", e.cut ? "{\"error\":\"Controller snapshot overflow\"}" : out);
}
void handle_state(ControlExchange &io) { send_state(io, false); }
void handle_diagnostics_get() { ControlExchange io(server); send_state(io, true); }
constexpr const char *WARNING_NAMES[] = {
    "low_battery", "overload", "temperature", "dc_ripple", "vebus_error", "mains_lost", "voltage_sense",
    "temperature_sense", "high_dc_voltage", "high_dc_current", "ac_not_accepted"
};
static_assert(sizeof WARNING_NAMES / sizeof WARNING_NAMES[0] == warnings::KIND_COUNT, "Update warning names");

void warning_history_json(Out &o, bool include_names = false) {
    const auto &history = listener_warning_history();
    o.add("{\"revision\":%u,", unsigned(history.revision));
    if (include_names) {
        o.add("\"kind_names\":[");
        for (unsigned i = 0; i < warnings::KIND_COUNT; ++i) {
            o.add("%s\"%s\"", i ? "," : "", WARNING_NAMES[i]);
        }
        o.add(R"(],"level_names":{"1":"warning","2":"alarm"},"timestamp_fields":["unix_s_or_zero","uptime_s"],)");
    }
    o.add("\"entries\":[");
    for (uint32_t i = history.data.count; i > 0; --i) {
        const auto &e = history.data.entries[i - 1];
        o.add("%s{\"kind\":%u,\"level\":%u,\"code\":%u,\"appeared\":[%u,%u],\"cleared\":",
              i == history.data.count ? "" : ",", unsigned(e.kind), unsigned(e.level), unsigned(e.code),
              unsigned(e.appeared.utc), unsigned(e.appeared.uptime_s));
        if (e.closed) o.add("[%u,%u]}", unsigned(e.cleared.utc), unsigned(e.cleared.uptime_s));
        else o.add("null}");
    }
    o.add("]}");
}

void handle_warning_history_get() {
    Out o(control_body, CONTROL_BODY_BYTES);
    warning_history_json(o);
    server.sendHeader("Cache-Control", "no-store");
    server.send_P(o.cut ? 500 : 200, "application/json", o.cut ? "{\"error\":\"Warning history overflow\"}" : control_body);
}
void handle_fault_report_read() {
    if(server.args()!=0 && !(server.args()==1 && server.hasArg("plain") && server.arg("plain").isEmpty())){
        server.send(400,"application/json","{\"error\":\"This read accepts no parameters\"}");return;
    }
    char why[160]="",json[256];
    bool ok=listener_fault_report_read(why,sizeof why);
    Out o(json,sizeof json);o.add("{\"ok\":%s,\"%s\":",tf(ok),ok?"message":"error");
    o.str(ok?"Reading fault report; no settings or history will be changed":why);o.add("}");
    server.sendHeader("Cache-Control","no-store");server.send(ok?202:409,"application/json",json);
}

void ess_json(Out &o) {
    listener_ess(view);
    o.add("{\"settings\":");
    settings_json(view.settings, o);
    o.add(",\"status\":");
    status_json(view, o);
    o.add("}");
}

// GET /api/ess: {"settings":{...},"status":{...}}
void handle_ess_get(ControlExchange &io) {
    Out o(control_body, CONTROL_BODY_BYTES);
    ess_json(o);
    io.sendHeader("Cache-Control", "no-store");
    io.send(o.cut ? 500 : 200, "application/json", o.cut ? "{\"error\":\"ESS status overflow\"}" : control_body);
}

void mcp_power_json(Out &o) {
    vebus::State state;
    listener_state(state, nullptr);
    const int64_t now = esp_timer_get_time();
    const bool bus_fresh = state.bus_fresh(now);
    auto sample = [&](const char *key, const vebus::Reading &reading, float sign = 1.f,
                      int64_t freshness_us = 5000000) {
        o.add(",\"%s\":", key);
        if (bus_fresh && warnings::fresh(reading.t, now, freshness_us) && isfinite(reading.v)) {
            o.add("{\"value\":%.2f,\"age_s\":%.2f}", double(sign * reading.v), double(now - reading.t) / 1e6);
        } else {
            o.add("null");
        }
    };
    o.add("{\"uptime_s\":%.2f,\"vebus_fresh\":%s", double(now) / 1e6, tf(bus_fresh));
    sample("ac_input_w_import_positive", state.acin_w, -1);
    sample("ac_input_v", state.mains_v);
    sample("ac_input_a", state.mains_a);
    sample("ac_output_net_w", state.acout_w);
    sample("ac_output_v", state.inverter_v);
    sample("ac_frequency_hz", state.ac_hz);
    sample("battery_w_charge_positive", state.inverter_w);
    sample("battery_a_charge_positive", state.bat_a);
    sample("battery_v", state.bat_v);
    sample("multiplus_soc_percent", state.soc, 1, 60000000);
    o.add("}");
}

bool mcp_diagnostics_json(Out &o) {
    diagnostics::Cache cached;
    listener_diagnostics(cached);
    char part[1800];
    const auto &live = listener_warning_status();
    o.add("{\"build\":\"%s\",\"uptime_s\":%.2f,\"wifi_rssi_dbm\":%d,\"wifi_disconnects\":%u,"
          "\"wifi_rejoins\":%u,\"gateway_verified\":%s,\"controller_state\":",
          diagnostics::BUILD, double(cached.now) / 1e6, int(WiFi.RSSI()), disconnect_count.load(),
          unsigned(wifi_cycles), tf(wifi_policy.confirmed()));
    listener_ess(view);
    o.str(view.ctl);
    o.add(",\"mains_lost\":%s,\"vebus_led_error\":", live.mains_lost < 0 ? "null" : tf(live.mains_lost));
    if (live.led_code > 0) o.add("%d", live.led_code); else o.add("null");
    o.add(",\"warnings\":{");
    for (unsigned i = 0; i < warnings::KIND_COUNT; ++i) {
        o.add("%s\"%s\":", i ? "," : "", WARNING_NAMES[i]);
        switch (live.levels[i]) {
        case warnings::CLEAR: o.str("clear"); break;
        case warnings::PREALARM: o.str("warning"); break;
        case warnings::ALARM: o.str("alarm"); break;
        default: o.add("null"); break;
        }
    }
    o.add("}");
    if (!diagnostics::to_json(cached, part, sizeof part)) return false;
    o.add(",\"cached\":%s", part);
    if (!faults::to_json(cached.faults, cached.now, cached.led_code, part, sizeof part)) return false;
    o.add(",\"fault_report\":%s", part);
    if (!charger::to_json(cached.charger, part, sizeof part)) return false;
    o.add(",\"charger\":%s}", part);
    return !o.cut;
}

bool mcp_snapshot(mcp::Tool tool, char *out, size_t size) {
    Out o(out, size);
    switch (tool) {
    case mcp::Tool::POWER_FLOW: mcp_power_json(o); break;
    case mcp::Tool::ESS: ess_json(o); break;
    case mcp::Tool::DIAGNOSTICS: return mcp_diagnostics_json(o);
    case mcp::Tool::WARNING_HISTORY: warning_history_json(o, true); break;
    }
    return !o.cut && o.n;
}

// POST /api/ess, form-encoded, any of the fields settings_from_form() reads.
void handle_ess_post(ControlExchange &io) {
    char why[120] = "", reply[200];
    listener_ess(view);
    ess::Settings s = view.settings;
    bool ok = settings_from_form(io, s, why, sizeof why) &&
              listener_ess_set(s, why, sizeof why, true, io.hasArg("tz"));
    if (ok) {
        snprintf(reply, sizeof reply, "{\"ok\":true,\"message\":\"saved: ESS from here %s, %s\"}",
                 s.enabled ? "on" : "off", ess::mode_name(s.mode));
    } else {
        snprintf(reply, sizeof reply, "{\"ok\":false,\"error\":\"%s\"}", why);
    }
    io.sendHeader("Cache-Control", "no-store");
    io.send(ok ? 200 : 400, "application/json", reply);
}

// ---------------------------------------------------------------- the Multi's settings

void handle_multi_page() { server.send_P(200, "text/html", MULTI_HTML); }

// Copy the settings once so HTTP responses never hold the VE.Bus state lock.
std::unique_ptr<vebus::Requester::SettingsRead> settings_snapshot(InverterSetupView *setup = nullptr) {
    std::unique_ptr<vebus::Requester::SettingsRead> snapshot(new (std::nothrow) vebus::Requester::SettingsRead);
    if (snapshot) listener_settings(*snapshot, setup);
    else server.send(503, "application/json", "{\"error\":\"Not enough memory for a settings snapshot\"}");
    return snapshot;
}

uint8_t *const settings_file = reinterpret_cast<uint8_t *>(control_body);

// GET /api/settings: the settings read so far, each as [id, state, value, scale, offset, default, min,
// max, access]; state 0 not read yet, 1 not supported, 2 read, 3 failed. "file" says whether the
// .rvsc can be built, "file_problem" why not.
void handle_settings_get() {
    InverterSetupView setup;
    auto snapshot = settings_snapshot(&setup);
    if (!snapshot) return;
    const auto &r = *snapshot;
    int64_t end = r.finished >= 0 ? r.finished : esp_timer_get_time();
    const char *why = "";
    bool file = rvsc::ready(r, &why);
    Out o(control_body, CONTROL_BODY_BYTES);
    o.add("{\"active\":%s,\"done\":%s,\"part\":\"%s\",\"step\":%d,\"progress\":%d,\"total\":%d,\"serial\":",
          tf(r.active), tf(r.done), vebus::Requester::part_name(r.part), r.step, vebus::Requester::progress(r),
          vebus::Requester::SETTINGS);
    o.str(r.serial);
    o.add(",\"firmware\":%lu,\"elapsed_s\":%.1f,\"read\":%d,\"unsupported\":%d,\"failed\":%d,\"grid_records\":%d,"
          "\"assistant_records\":%d,\"file\":%s,\"file_problem\":",
          (unsigned long)r.firmware, r.started < 0 ? 0.0 : (end - r.started) / 1e6, r.read, r.unsupported, r.failed,
          r.grid.count, r.assistants.count, tf(file));
    o.str(why);
    const bool legacy = r.grid_availability == vebus::Requester::GridAvailability::LEGACY_OMITTED;
    o.add(",\"version_bytes\":%u,\"backup_schema_version\":%u,\"grid_metadata\":\"%s\"",
          unsigned(r.version_bytes), legacy ? 2u : 4u,
          legacy ? "omitted_by_legacy_format" : r.grid.ended ? "device_records_complete" : "incomplete");
    o.add(",\"inspection\":%s,\"setup\":{\"status\":\"%s\",\"model\":", tf(r.inspection), setup.status);
    o.str(setup.model);
    o.add(",\"nominal_voltage\":%u,\"ess_setpoint_ram\":%u,\"can_upload\":%s,\"problem\":",
          unsigned(setup.nominal_voltage), unsigned(setup.ess_setpoint_ram), tf(setup.can_upload));
    o.str(r.inspection && r.problem ? r.problem : "");
    o.add("},\"settings\":[");
    for (int i = 0; i < vebus::Requester::SETTINGS; i++) {
        const vebus::Requester::Setting &x = r.s[i];
        o.add("%s[%d,%u,%u,%d,%d,%u,%u,%u,%u]", i ? "," : "", i, x.state, x.value, x.scale, x.offset, x.def, x.min,
              x.max, x.access);
    }
    o.add("]}");
    snapshot.reset();
    server.sendHeader("Cache-Control", "no-store");
    server.send_P(o.cut ? 500 : 200, "application/json", o.cut ? "{\"error\":\"too long\"}" : control_body);
}

// GET /api/settings.rvsc: the read as the VE.Bus settings file Venus's backup and VRM's Remote
// VEConfigure download write (rvsc.h), stamped with the time the read ended (0 if NTP had not set
// the clock).
void handle_settings_file() {
    auto snapshot = settings_snapshot();
    if (!snapshot) return;
    const auto &r = *snapshot;
    const char *why = "";
    server.sendHeader("Cache-Control", "no-store");
    if (!rvsc::ready(r, &why)) {
        char reply[160];
        snprintf(reply, sizeof reply, "{\"ok\":false,\"error\":\"no file: %s\"}", why);
        server.send(409, "application/json", reply);
        return;
    }
    time_t now = time(nullptr);
    uint32_t saved = now > 1767225600 ? (uint32_t)(now - (esp_timer_get_time() - r.finished) / 1000000) : 0;
    size_t n = rvsc::build(r, saved, settings_file, CONTROL_BODY_BYTES);
    if (n == 0) {
        server.send(500, "application/json", "{\"ok\":false,\"error\":\"the file does not fit\"}");
        return;
    }
    char disposition[80];
    snprintf(disposition, sizeof disposition, "attachment; filename=\"multi-settings-%s.rvsc\"", r.serial);
    snapshot.reset();
    server.sendHeader("Content-Disposition", disposition);
    server.send_P(200, "application/octet-stream", control_body, n);
}

// GET /api/compatibility.json: the public profile derived from a complete settings read.
void handle_compatibility_profile() {
    server.sendHeader("Cache-Control", "no-store");
    auto snapshot = settings_snapshot();
    if (!snapshot) return;

    compatibility_profile::Profile profile;
    if (!compatibility_profile::prepare(*snapshot, profile, nullptr)) {
        server.send(409, "application/json",
                    "{\"error\":\"This inverter's settings cannot be exported as a compatibility profile.\"}");
        return;
    }

    // Hex-encoded grid descriptors can outgrow the shared response buffer.
    server.sendHeader("Content-Disposition", "attachment; filename=\"compatibility.json\"");
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "application/json", "");
    const bool sent = compatibility_profile::write_json(*snapshot, profile,
        [](const char *data, size_t length, void *) -> bool {
            if (!server.client().connected()) return false;
            server.sendContent(data, length);
            return server.client().connected();
        }, nullptr);
    if (sent) server.sendContent("", 0);
    else server.client().stop();
}

// POST /api/settings/read: start reading every setting.
void handle_settings_read() {
    const bool inspection = server.arg("inspect") == "1";
    bool ok = listener_settings_read(inspection);
    server.sendHeader("Cache-Control", "no-store");
    server.send(ok ? 200 : 409, "application/json",
                ok ? "{\"ok\":true,\"message\":\"reading the Multi's settings\"}"
                   : "{\"ok\":false,\"error\":\"Wait for brain_zero to have a clear VE.Bus connection and for the current operation to finish\"}");
}

// ---------------------------------------------------------------- writing a settings file

size_t upload_len = 0;
bool upload_too_big = false;
char upload_name[48] = "";
web::UploadView upload_view;

// GET /api/upload, and the answer to the other upload requests: the file, what it would change against
// the Multi's latest read, the bus, and the write job.
void send_upload(int code, const char *error) {
    UploadView &v = upload_view;
    listener_upload(v);
    Out o(control_body, CONTROL_BODY_BYTES);
    o.add("{\"ok\":%s", tf(error == nullptr));
    if (error) {
        o.add(",\"error\":");
        o.str(error);
    }
    o.add(",\"state\":\"%s\",\"busy\":%s,\"message\":", v.state, tf(v.busy));
    o.str(v.message);
    o.add(",\"file\":{\"name\":");
    o.str(v.name);
    o.add(",\"bytes\":%u,\"serial\":", v.bytes);
    o.str(v.serial);
    o.add(",\"firmware\":%lu,\"saved\":%lu,\"form\":\"%s\",\"mk2vsc\":", (unsigned long)v.firmware,
          (unsigned long)v.saved, v.upload_form ? "upload" : "device");
    o.str(v.mk2vsc);
    o.add(",\"credentials\":%u},\"read\":{\"done\":%s,\"active\":%s,\"age_s\":%.0f},\"check\":{\"done\":%s,\"ok\":%s,\"why\":", v.credentials, tf(v.read_done),
          tf(v.read_active), v.read_age_s, tf(v.checked), tf(v.check_ok));
    o.str(v.check_why);
    o.add(",\"protected\":%s,\"restart\":%s,\"settings\":[", tf(v.protected_changes), tf(v.restart_required));
    for (int i = 0; i < v.settings; i++) o.add("%s[%u,%u,%u]", i ? "," : "", v.id[i], v.from[i], v.to[i]);
    o.add("],\"assistants\":%s,\"assistant_bytes\":%d,\"assistant_first\":%d,\"removes_assistants\":%s,"
          "\"adds_assistants\":%s},\"bus\":{\"master\":%s,\"ctl\":\"%s\"},\"write\":{\"active\":%s,\"step\":\"%s\","
          "\"progress\":%d,\"settings_written\":%d,\"records_written\":%d,\"index\":%d,"
          "\"access_only\":%s,\"authorized\":%u,\"access_requests\":%u,\"access_reply\":%d,\"config_sends\":%u,\"reset_sends\":%u}}",
          tf(v.assistants), v.assistant_bytes_differ, v.assistant_first_differ, tf(v.removes_assistants),
          tf(v.adds_assistants), tf(v.master), v.ctl, tf(v.write_active), v.write_step, v.write_progress,
          v.settings_written, v.records_written, v.write_index, tf(v.access_only), v.authorized,
          v.access_requests, v.access_reply, v.config_sends, v.reset_sends);
    server.sendHeader("Cache-Control", "no-store");
    server.send_P(o.cut ? 500 : code, "application/json", o.cut ? "{\"ok\":false,\"error\":\"too long\"}" : control_body);
}

void handle_upload_get() { send_upload(200, nullptr); }

// POST /api/upload gathers the multipart file in the shared buffer before writing the response.
void handle_upload_chunk() {
    HTTPUpload &up = server.upload();
    if (up.status == UPLOAD_FILE_START) {
        upload_len = 0;
        upload_too_big = false;
        snprintf(upload_name, sizeof upload_name, "%s", up.filename.c_str());
    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (upload_len + up.currentSize > CONTROL_BODY_BYTES) {
            upload_too_big = true;
        } else if (!upload_too_big) {
            memcpy(settings_file + upload_len, up.buf, up.currentSize);
            upload_len += up.currentSize;
        }
    }
}

void handle_upload_post() {
    char why[160] = "";
    bool ok = false;
    if (upload_too_big) {
        snprintf(why, sizeof why, "the file is larger than %u bytes: not a single unit's .rvsc", (unsigned)CONTROL_BODY_BYTES);
    } else if (upload_len == 0) {
        snprintf(why, sizeof why, "no file received");
    } else {
        ok = listener_upload_load(settings_file, upload_len, upload_name, why, sizeof why);
    }
    vebus::erase_secret(settings_file, upload_len);
    upload_len = 0;
    send_upload(ok ? 200 : 400, ok ? nullptr : why);
}

// POST /api/upload/write, /cancel and /clear.
void handle_upload_write() {
    char why[160];
    bool ok = listener_upload_start(why, sizeof why, false);
    send_upload(ok ? 200 : 409, ok ? nullptr : why);
}

// Separate diagnostic endpoint: sends embedded authorization only, never the file's changes.
void handle_upload_test_access() {
    char why[160];
    bool ok = listener_upload_start(why, sizeof why, true);
    send_upload(ok ? 200 : 409, ok ? nullptr : why);
}

void handle_upload_cancel() {
    char why[160];
    bool ok = listener_upload_cancel(why, sizeof why);
    send_upload(ok ? 200 : 409, ok ? nullptr : why);
}

void handle_upload_clear() {
    listener_upload_clear();
    send_upload(200, nullptr);
}

// ---------------------------------------------------------------- MQTT

void handle_mqtt_page() { server.send_P(200, "text/html", MQTT_HTML); }

void battery_input_json(Out &o) {
    external_battery::View v; listener_battery_input(v);
    o.add("{\"enabled\":%s,\"connected\":%s,\"soc_valid\":%s,\"current_valid\":%s,\"voltage_valid\":%s,\"soc\":",
          tf(v.enabled),tf(v.connected),tf(v.soc_valid),tf(v.current_valid),tf(v.voltage_valid));
    if(v.soc_valid)o.add("%.2f",v.soc);else o.add("null");
    o.add(",\"voltage\":");if(v.voltage_valid)o.add("%.2f",v.voltage);else o.add("null");
    o.add(",\"external_current_a\":");if(v.current_valid)o.add("%.2f",v.external_current_a);else o.add("null");
    o.add(",\"soc_age_s\":%.3f,\"current_age_s\":%.3f,\"accepted\":%u,\"refused\":%u,\"topic\":",
          v.soc_age_s,v.current_age_s,v.accepted,v.refused);
    o.str(mqtt_link::battery_topic());
    o.add(",\"sync_verified\":%s,\"sync_target\":",tf(v.sync_verified));
    if(v.sync_sent)o.add("%.1f",v.sync_target);else o.add("null");
    o.add(",\"sync_age_s\":%.3f,\"sync_sent\":%u,\"sync_current_sent\":%u}",
          v.sync_age_s,v.sync_sent,v.sync_current_sent);
}
void handle_battery_input_get(ControlExchange &io) {
    Out o(control_body,CONTROL_BODY_BYTES);battery_input_json(o);
    io.sendHeader("Cache-Control","no-store");io.send(o.cut?500:200,"application/json",control_body);
}
void handle_battery_input_post(ControlExchange &io) {
    long enabled; Fields fields(io); char why[120]="";
    if(io.args()!=1 || !fields.get_long("enabled",&enabled,0,1) || fields.bad[0]) {
        io.send(400,"application/json","{\"error\":\"The request was refused.\"}");return;
    }
    if(!listener_battery_input_enabled(enabled!=0,why,sizeof why)) {
        Out o(control_body,CONTROL_BODY_BYTES);o.add("{\"error\":");o.str(why);o.add("}");
        io.send(500,"application/json",control_body);return;
    }
    handle_battery_input_get(io);
}
void handle_ess_lease_get(ControlExchange &io) {
    ess_lease::View v;listener_ess_lease(v);
    Out o(control_body,CONTROL_BODY_BYTES);
    o.add("{\"active\":%s,\"id\":",tf(v.active));o.str(v.id);
    o.add(",\"charge_w\":%d,\"remaining_s\":%u}",v.charge_w,v.remaining_s);
    io.sendHeader("Cache-Control","no-store");io.send(o.cut?500:200,"application/json",control_body);
}
void handle_ess_lease_post(ControlExchange &io) {
    const String id=io.arg("id");Fields fields(io);long ttl=0,watts=0,clear=0;
    const bool valid_id=id.length()==32 && strspn(id.c_str(),"0123456789abcdef")==32;
    const char *error=nullptr;
    if(!valid_id)error="The request was refused.";
    else if(io.hasArg("clear")) {
        if(io.args()!=2 || !fields.get_long("clear",&clear,1,1) || fields.bad[0])error="The request was refused.";
        else error=listener_ess_lease_clear(id.c_str());
    } else {
        if(io.args()!=3 || !fields.get_long("ttl_s",&ttl,5,60) ||
           !fields.get_long("charge_w",&watts,0,5000) || fields.bad[0])error="The request was refused.";
        else error=listener_ess_lease_set(id.c_str(),uint32_t(ttl),int(watts));
    }
    if(error) {
        Out o(control_body,CONTROL_BODY_BYTES);o.add("{\"error\":");o.str(error);o.add("}");
        io.send(!strcmp(error,"Another controller is active")?409:400,"application/json",control_body);return;
    }
    handle_ess_lease_get(io);
}

// GET /api/mqtt: {"settings":{...},"status":{...}}; the password is never sent, only whether one is set.
void handle_mqtt_get() {
    ha::Settings m;
    listener_mqtt(m);
    mqtt_link::status_json(json_fragment, sizeof json_fragment);
    Out o(control_body, CONTROL_BODY_BYTES);
    o.add("{\"settings\":{\"enabled\":%d,\"host\":", m.enabled);
    o.str(m.host);
    o.add(",\"port\":%u,\"user\":", m.port);
    o.str(m.user);
    o.add(",\"password_set\":%s,\"base\":\"%s\",\"discovery\":%d,\"prefix\":\"%s\",\"interval_s\":%u},"
          "\"entities\":%d,\"status\":%s,\"battery_input\":",
          m.password[0] ? "true" : "false", m.base, m.discovery, m.prefix, m.interval_s, ha::entity_count(), json_fragment);
    battery_input_json(o);o.add("}");
    server.sendHeader("Cache-Control", "no-store");
    server.send_P(200, "application/json", control_body);
}

// POST /api/mqtt, form-encoded: enabled, host, port, user, password (left out or empty: unchanged),
// password_clear=1, base, discovery, prefix, interval_s. Saved, and the client reconnects with them.
void handle_mqtt_post() {
    char why[120] = "", reply[200];
    ha::Settings m;
    listener_mqtt(m);
    ControlExchange io(server);
    Fields f(io);
    f.get("enabled", &m.enabled);
    f.get_text("host", m.host, sizeof m.host);
    long port;
    if (f.get_long("port", &port, 1, 65535)) m.port = (uint16_t)port;
    f.get_text("user", m.user, sizeof m.user);
    if (server.hasArg("password") && server.arg("password").length() > 0) f.get_text("password", m.password, sizeof m.password);
    if (server.arg("password_clear") == "1") m.password[0] = 0;
    f.get_text("base", m.base, sizeof m.base);
    f.get("discovery", &m.discovery);
    f.get_text("prefix", m.prefix, sizeof m.prefix);
    long interval;
    if (f.get_long("interval_s", &interval, 1, 3600)) m.interval_s = (uint16_t)interval;
    external_battery::View battery;listener_battery_input(battery);
    bool external_input=battery.enabled;f.get("external_input",&external_input);
    bool ok;
    if (f.bad[0]) {
        snprintf(why, sizeof why, "bad value for %s", f.bad);
        ok = false;
    } else if (!ha::validate(m,why,sizeof why)) {
        ok=false;
    } else if (server.hasArg("external_input") && !listener_battery_input_enabled(external_input,why,sizeof why)) {
        ok=false;
    } else {
        ok = listener_mqtt_set(m, why, sizeof why);
    }
    if (ok) {
        snprintf(reply, sizeof reply, "{\"ok\":true,\"message\":\"saved; %s\"}",
                 m.enabled ? "connecting to the broker" : "MQTT is off");
    } else {
        snprintf(reply, sizeof reply, "{\"ok\":false,\"error\":\"%s\"}", why);
    }
    server.sendHeader("Cache-Control", "no-store");
    server.send(ok ? 200 : 400, "application/json", reply);
}

}  // namespace

namespace {
using ControlHandler = void (*)(ControlExchange &);
struct ControlRoute {
    const char *name;
    ControlHandler read;
    ControlHandler write;
};

const ControlRoute CONTROL_ROUTES[] = {
    {"state", handle_state, nullptr},
    {"ess", handle_ess_get, handle_ess_post},
    {"ess/lease", handle_ess_lease_get, handle_ess_lease_post},
    {"battery-input", handle_battery_input_get, handle_battery_input_post},
    {"inverter", handle_inverter_get, handle_inverter_post},
    {"charger", handle_charger_get, handle_charger_post},
    {"charger/read", nullptr, handle_charger_read},
    {"assist", handle_assist_get, handle_assist_post},
    {"assist/read", nullptr, handle_assist_read},
    {"system", handle_system_get, handle_system_post},
    {"ac-input", nullptr, [](ControlExchange &io) { handle_ac_input_post(io, false); }},
    {"ac-input/settings", nullptr, [](ControlExchange &io) { handle_ac_input_post(io, true); }},
    {"pv-disable", nullptr, [](ControlExchange &io) { handle_pv_post(io, false); }},
    {"pv-disable/settings", nullptr, [](ControlExchange &io) { handle_pv_post(io, true); }},
};
}

const char *controller_request(const char *name, bool write, const cJSON *fields) {
    ControlExchange io(fields, control_body, CONTROL_BODY_BYTES);
    io.send(404, "application/json", "{\"error\":\"Unsupported controller operation\"}");
    for (const auto &route : CONTROL_ROUTES) {
        if (strcmp(route.name, name)) continue;
        const auto handle = write ? route.write : route.read;
        if (handle) handle(io);
        break;
    }
    const size_t len = strlen(control_body);
    const int header = snprintf(control_response, MQTT_HEADER_BYTES, "{\"status\":%d,\"data\":", io.status);
    memmove(control_response + header, control_body, len);
    control_response[header + len] = '}';
    control_response[header + len + 1] = 0;
    return control_response;
}

bool local_host(const String &host) {
    return host.equalsIgnoreCase(String(HOSTNAME) + ".local") ||
           host.equalsIgnoreCase(String(LEGACY_HOSTNAME) + ".local") || host == WiFi.localIP().toString();
}

void begin() {
    ui_prefs.begin("brain-ui", false);
    console_auth::begin(ui_prefs);
    language = ui_prefs.getString("language", "en");
    if (!ui_language(language.c_str())) language = "en";
    device_settings::Network saved;
    if (ui_prefs.getBytesLength("wifi") == sizeof saved &&
        ui_prefs.getBytes("wifi", &saved, sizeof saved) == sizeof saved && device_settings::valid(saved)) {
        network = saved;
    } else {
        snprintf(network.ssid, sizeof network.ssid, "%s", WIFI_SSID);
        snprintf(network.password, sizeof network.password, "%s", WIFI_PASSWORD);
    }
    if (!device_settings::valid(network)) return;
    enabled = true;
    if (esp_reset_reason() != ESP_RST_SW) {
        wifi_restart_cookie = 0;
        wifi_gateway_cookie = 0;
    }
    if (wifi_gateway_cookie == WIFI_GATEWAY_VERIFIED && wifi_verified_gateway) {
        wifi_policy.observe_gateway(wifi_verified_gateway);
        wifi_policy.probe(true);
    }
    WiFi.persistent(false);
    WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
        if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
            const auto &lost = info.wifi_sta_disconnected;
            if (lost.reason != WIFI_REASON_ASSOC_LEAVE) {
                disconnect_reason.store(lost.reason);
                ++disconnect_count;
                Serial0.printf("# wifi: disconnected reason=%u ap=%02X:%02X:%02X:%02X:%02X:%02X\n",
                               unsigned(lost.reason), lost.bssid[0], lost.bssid[1], lost.bssid[2],
                               lost.bssid[3], lost.bssid[4], lost.bssid[5]);
            }
            ++wifi_epoch;
        } else if (event == ARDUINO_EVENT_WIFI_STA_CONNECTED) {
            const auto &joined = info.wifi_sta_connected;
            int8_t tx_power = 0;
            esp_wifi_get_max_tx_power(&tx_power);
            uint8_t protocols = 0;
            esp_wifi_get_protocol(WIFI_IF_STA, &protocols);
            wifi_bandwidth_t width = WIFI_BW_HT20;
            const bool width_known = esp_wifi_get_bandwidth(WIFI_IF_STA, &width) == ESP_OK;
            Serial0.printf("# wifi: associated ap=%02X:%02X:%02X:%02X:%02X:%02X channel=%u auth=%u tx_qdbm=%d width=%u protocols=0x%02X\n",
                           joined.bssid[0], joined.bssid[1], joined.bssid[2], joined.bssid[3],
                           joined.bssid[4], joined.bssid[5], unsigned(joined.channel),
                           unsigned(joined.authmode), int(tx_power), width_known ? (width == WIFI_BW_HT20 ? 20u : 40u) : 0u,
                           unsigned(protocols));
        } else if (event == ARDUINO_EVENT_WIFI_STA_LOST_IP || event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
            ++wifi_epoch;
        }
    });
    server.on("/brand/brain-zero.svg", HTTP_GET, [] {
        server.sendHeader("Cache-Control", "no-cache");
        server.send_P(200, "image/svg+xml", BRAND_LOGO);
    });
    server.on("/brand/brain-zero-dark.svg", HTTP_GET, [] {
        server.sendHeader("Cache-Control", "no-cache");
        server.send_P(200, "image/svg+xml", BRAND_LOGO_DARK);
    });
    server.on("/brand/brain-zero-icon.svg", HTTP_GET, [] {
        server.sendHeader("Cache-Control", "no-cache");
        server.send_P(200, "image/svg+xml", BRAND_ICON);
    });
    server.on("/brand/brand.css", HTTP_GET, [] {
        server.sendHeader("Cache-Control", "no-cache");
        server.send_P(200, "text/css", BRAND_CSS);
    });
    server.on("/ui/settings.css", HTTP_GET, [] { server.send_P(200, "text/css", DEVICE_CSS); });
    server.on("/ui/language.js", HTTP_GET, [] {
        server.sendHeader("Cache-Control", "no-store");
        server.send_P(200, "application/javascript", ui_language(language.c_str())->script);
    });
    server.on("/ui/translate.js", HTTP_GET, [] { server.send_P(200, "application/javascript", UI_TRANSLATE_JS); });
    server.on("/login", HTTP_GET, [] {
        if (!console_auth::enabled() || console_auth::access(server.header("Cookie"), "", millis()) == console_auth::Access::SESSION) {
            redirect("/"); return;
        }
        server.sendHeader("Cache-Control", "no-store");
        server.sendHeader("X-Frame-Options", "DENY");
        server.send_P(200, "text/html", LOGIN_HTML);
    });
    server.on("/login", HTTP_POST, handle_login);
    server.on("/logout", HTTP_POST, handle_logout);
    private_route("/settings", HTTP_GET, [] {
        server.sendHeader("Cache-Control", "no-store");
        server.sendHeader("X-Frame-Options", "DENY");
        server.send_P(200, "text/html", DEVICE_HTML);
    });
    private_route("/api/device", HTTP_GET, handle_device);
    private_route("/api/device/language", HTTP_POST, handle_language);
    private_route("/api/device/timezones", HTTP_GET, [] {
        server.send_P(200, "application/json", controller_time::names_json());
    });
    private_route("/api/device/timezone", HTTP_POST, handle_time_zone);
    private_route("/api/device/password", HTTP_POST, handle_console_password);
    private_route("/api/device/token", HTTP_POST, handle_console_token);
    private_route("/api/device/wifi", HTTP_POST, handle_network_test);
    private_route("/api/device/wifi/keep", HTTP_POST, handle_network_keep);
    private_route("/api/device/wifi/revert", HTTP_POST, handle_network_revert);
    private_route("/", HTTP_GET, handle_root);
    private_route("/diagnostics",HTTP_GET,[]{server.send_P(200,"text/html",DIAGNOSTICS_HTML);});
    private_route("/api/diagnostics",HTTP_GET,handle_diagnostics_get);
    private_route("/api/diagnostics/fault-report/read",HTTP_POST,handle_fault_report_read);
    private_route("/ess", HTTP_GET, handle_ess_page);
    private_route("/inverter", HTTP_GET, handle_inverter_page);
    private_route("/api/ac-input",HTTP_GET,handle_ac_input_get);
    private_route("/api/pv-disable",HTTP_GET,handle_pv_get);
    private_route("/firmware", HTTP_GET, [] {
        if (!firmware_host()) { server.send(403, "text/plain", "Use the controller's local address."); return; }
        server.sendHeader("Cache-Control", "no-store");
        server.sendHeader("X-Frame-Options", "DENY");
        server.send_P(200, "text/html", FIRMWARE_HTML);
    });
    private_route("/api/firmware", HTTP_GET, [] {
        const bool local = firmware_host();
        firmware_reply(local ? 200 : 403, local ? nullptr : "Use the controller's local address.");
    });
    private_route("/api/firmware/release", HTTP_GET, [] {
        if (!firmware_host()) { firmware_release_reply(403, "Use the controller's local address."); return; }
        firmware_release_reply(200);
    });
    private_route("/api/firmware/release/check", HTTP_POST, firmware_release_check);
    private_route("/api/firmware/release/stage", HTTP_POST, firmware_release_stage);
    private_route("/api/firmware/upload", HTTP_POST, finish_firmware_upload, firmware_upload_chunk);
    private_route("/api/firmware/install", HTTP_POST, [] { firmware_action(firmware_ota::Action::INSTALL); });
    private_route("/api/firmware/rollback", HTTP_POST, [] { firmware_action(firmware_ota::Action::ROLLBACK); });
    private_route("/api/firmware/keep", HTTP_POST, [] {
        const char *why = firmware_access();
        if (!why && firmware_release_job::busy()) why = OTA_BUSY;
        if (!why) why = firmware_ota::keep();
        firmware_reply(why ? 409 : 200, why);
    });
    private_route("/api/firmware/discard", HTTP_POST, [] {
        const char *why = firmware_access();
        if (!why && firmware_release_job::busy()) why = OTA_BUSY;
        if (!why) {
            const auto s = firmware_ota::status();
            if (!s.staged || s.queued != firmware_ota::Action::NONE) why = "No staged image can be discarded.";
            else {
                firmware_ota::discard("Staged image discarded. Current firmware will continue to run.");
                firmware_release_job::discarded();
            }
        }
        firmware_reply(why ? 409 : 200, why);
    });
    private_route("/api/warning-history", HTTP_GET, handle_warning_history_get);
    private_route("/mqtt", HTTP_GET, [] { redirect("/settings/mqtt"); });
    private_route("/settings/mqtt", HTTP_GET, handle_mqtt_page);
    private_route("/multi", HTTP_GET, handle_multi_page);
    private_route("/api/settings", HTTP_GET, handle_settings_get);
    private_route("/api/settings.rvsc", HTTP_GET, handle_settings_file);
    private_route("/api/compatibility.json", HTTP_GET, handle_compatibility_profile);
    private_route("/api/settings/read", HTTP_POST, handle_settings_read);
    private_route("/api/upload", HTTP_GET, handle_upload_get);
    private_route("/api/upload", HTTP_POST, handle_upload_post, handle_upload_chunk);
    private_route("/api/upload/write", HTTP_POST, handle_upload_write);
    private_route("/api/upload/test-access", HTTP_POST, handle_upload_test_access);
    private_route("/api/upload/cancel", HTTP_POST, handle_upload_cancel);
    private_route("/api/upload/clear", HTTP_POST, handle_upload_clear);
    private_route("/api/mqtt", HTTP_GET, handle_mqtt_get);
    private_route("/api/mqtt", HTTP_POST, handle_mqtt_post);
    for (const auto &route : CONTROL_ROUTES) {
        const String path = String("/api/") + route.name;
        const auto read = route.read, write = route.write;
        if (read) private_route(path.c_str(), HTTP_GET, [read] {
            ControlExchange io(server);
            read(io);
        });
        if (write) private_route(path.c_str(), HTTP_POST, [write] {
            ControlExchange io(server);
            write(io);
        });
    }
    mcp::register_route(server, mcp_snapshot, control_body, 4096, [] { return console_allowed(true); });
    server.onNotFound([] { server.send(404, "text/plain", "not found"); });
    const char *headers[] = {OTA_TOKEN, OTA_SIZE, "Host", "Origin", "Content-Type", "Cookie", "Sec-Fetch-Site",
                             "MCP-Protocol-Version", "Mcp-Method", "Mcp-Name"};
    server.collectHeaders(headers, sizeof headers / sizeof headers[0]);
    start_station();
}

void loop() {
    firmware_release_job::poll(listener_firmware_allowed);
    if (!enabled) return;
    const uint32_t now = millis();
    if (!restart_shutdown) {
        const auto action = network_trial.tick(now);
        if (action != device_settings::NetworkTrial::Action::NONE) {
            wifi_policy = wifi_recovery::Policy{};
            wifi_gateway_cookie = 0;
            if (action == device_settings::NetworkTrial::Action::REVERT) {
                candidate = {};
                network_notice = "Previous Wi-Fi network restored.";
            }
            pause_station(now);
        }
    }
    if (cycle == Cycle::DISCONNECTING) {
        if (now - cycle_at < wifi_recovery::DISCONNECT_MS) return;
        WiFi.mode(WIFI_OFF);
        cycle_at = now;
        cycle = Cycle::RADIO_OFF;
        return;
    }
    if (cycle == Cycle::RADIO_OFF) {
        if (restart_shutdown || now - cycle_at < wifi_recovery::RADIO_OFF_MS) return;
        cycle = Cycle::IDLE;
        start_station();
    }
    const IPAddress ip = WiFi.status() == WL_CONNECTED ? WiFi.localIP() : IPAddress();
    const bool has_ip = uint32_t(ip) != 0;
    const uint32_t epoch = wifi_epoch.load();
    if (!network_trial.pending()) probe_gateway_path(now, has_ip ? uint32_t(WiFi.gatewayIP()) : 0, epoch);
    if (!network_trial.pending() && wifi_policy.due(now, has_ip)) {
        if (wifi_policy.should_restart_controller() && wifi_restart_cookie != WIFI_RESTART_USED &&
            listener_wifi_restart()) {
            // Leave the AP and hold the radio off before rebooting, as with a USB reset.
            begin_restart_shutdown();
        } else {
            cycle_station(now, has_ip ? "gateway unreachable" : "no IPv4 address");
        }
        return;
    }
    if (wifi_policy.stable()) wifi_restart_cookie = 0;
    if (ip != served_ip || epoch != served_epoch) {
        stop_http();
        served_epoch = epoch;
        if (has_ip) {
            server.begin();
            served_ip = ip;
            mdns_at = now - MDNS_RETRY_MS;
        }
    }
    if (has_ip) {
        refresh_mdns(ip, now);
        server.handleClient();
        if (upload_len) {
            vebus::erase_secret(settings_file, upload_len);
            upload_len = 0;
        }
        if (firmware_upload.seen) {
            if (firmware_upload.owns) firmware_ota::discard("Firmware upload interrupted. Choose the file again.");
            firmware_upload = {};
        }
    }
}

void reconnect() {
    if (enabled) cycle_station(millis(), "USB request");
}

bool reset_wifi() {
    device_settings::Network fallback;
    snprintf(fallback.ssid, sizeof fallback.ssid, "%s", WIFI_SSID);
    snprintf(fallback.password, sizeof fallback.password, "%s", WIFI_PASSWORD);
    if (!enabled || restart_shutdown || !device_settings::valid(fallback) ||
        (ui_prefs.isKey("wifi") && !ui_prefs.remove("wifi"))) return false;
    network = fallback;
    candidate = {};
    network_trial.cancel(millis());
    return true;
}

bool reset_password() { return console_auth::reset(); }

void mark_recovery_restart() {
    wifi_restart_cookie = WIFI_RESTART_USED;
}

void begin_restart_shutdown() {
    restart_shutdown = true;
    Serial0.println("# wifi: leaving access point before controller restart");
    pause_station(millis());
}

bool recovery_restart_ready() {
    return restart_shutdown && cycle == Cycle::RADIO_OFF &&
           millis() - cycle_at >= wifi_recovery::RADIO_OFF_MS;
}

void cancel_recovery_restart() {
    restart_shutdown = false;
}

size_t describe(char *out, size_t cap) {
    int n;
    if (!enabled) {
        n = snprintf(out, cap, "wifi=off (no password in wifi_secrets.h)");
    } else if (WiFi.status() != WL_CONNECTED) {
        n = snprintf(out, cap, "wifi=connecting to %s", network_trial.testing() ? candidate.ssid : network.ssid);
    } else {
        n = snprintf(out, cap, "wifi=%s rssi=%d http://%s.local/", WiFi.localIP().toString().c_str(),
                     (int)WiFi.RSSI(), HOSTNAME);
    }
    return n > 0 ? (size_t)n : 0;
}

}  // namespace web
