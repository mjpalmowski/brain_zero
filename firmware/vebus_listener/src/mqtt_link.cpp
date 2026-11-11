#include "mqtt_link.h"

#include <Arduino.h>
#include <WiFi.h>
#include <atomic>

#include "esp_mac.h"
#include "esp_timer.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "victron_telemetry.h"
#include "vebus_state.h"
#include "web.h"

void listener_state(vebus::State &out);  // main.cpp
void listener_ess(web::EssView &out);    // main.cpp
bool listener_inverter_command(const char *key, const char *value, char *why, size_t cap);

namespace mqtt_link {

namespace {

struct Message {  // a command or Home Assistant's status, from the client's task to the Arduino loop
    char topic[160];
    char payload[1400];
    bool retained = false;
    int64_t received_at = 0;
};

ha::Settings cfg;  // what the client runs with; it keeps pointers into these strings
ha::Ids ids;
esp_mqtt_client_handle_t client = nullptr;
QueueHandle_t inbox = nullptr;
std::atomic<bool> up{false}, announce{false};
char status_topic[64], state_topic[64], command_filter[64], ha_status_topic[48];
char rpc_request_topic[48];
char recent_write_ids[16][33];
unsigned next_write_id = 0;
uint32_t last_rpc_ms = 0;
uint32_t published = 0, commands = 0, refused = 0;
uint32_t last_state_ms = 0, state_attempt_ms = 0;
bool state_now = false;
constexpr uint32_t WIFI_SETTLE_MS = 1500, DISCOVERY_GAP_MS = 180, GX_GAP_MS = 120;
constexpr int OUTBOX_LIMIT = 3072;
uint32_t wifi_ready_at = 0, discovery_at = 0, gx_batch_at = 0;
int discovery_cursor = -1;
char last_command[112] = "";
char last_error[112] = "";  // written by the client's task
portMUX_TYPE error_lock = portMUX_INITIALIZER_UNLOCKED;

gx::Identity gx_id;
char gx_read_filter[32];
struct Refresh { gx::Request request; bool respond = false; };
Refresh gx_pending[4], gx_active;
unsigned gx_head = 0, gx_size = 0;
int gx_cursor = -1;
gx::Snapshot gx_snapshot;
uint32_t gx_last_ms = 0, gx_published = 0, gx_reads = 0, gx_refused = 0;

bool gx_queue(const gx::Request &request, bool respond) {
    if (gx_size == 4) return false;
    auto &p = gx_pending[(gx_head + gx_size++) % 4]; p.request = request; p.respond = respond;
    return true;
}
bool gx_options(const char *payload, gx::Request &r) {
    if (!*payload) return true;
    cJSON *root = cJSON_ParseWithLengthOpts(payload, strlen(payload) + 1, nullptr, 1);
    if (!root) return false;
    bool ok = cJSON_IsObject(root);
    auto *options = cJSON_GetObjectItemCaseSensitive(root, "keepalive-options");
    if (options && !cJSON_IsArray(options)) ok = false;
    cJSON *option = nullptr;
    cJSON_ArrayForEach(option, options) {
        if (cJSON_IsString(option) && !strcmp(option->valuestring, "suppress-republish")) r.suppress = true;
        auto *echo = cJSON_GetObjectItemCaseSensitive(option, "full-publish-completed-echo");
        if (echo) {
            if (!cJSON_IsString(echo) || strlen(echo->valuestring) >= sizeof r.echo) { ok = false; break; }
            for (const char *p = echo->valuestring; *p; ++p) if ((unsigned char)*p < 32 || (unsigned char)*p > 126) ok = false;
            snprintf(r.echo, sizeof r.echo, "%s", echo->valuestring);
        }
    }
    cJSON_Delete(root); return ok;
}

// Bounded work in the application loop. No calls here can set ESS or inverter
// parameters; all outgoing topics come from gx's fixed notification whitelist.
void gx_publish() {
    uint32_t ms = millis();
    if (ms - gx_batch_at < GX_GAP_MS) return;
    if (gx_cursor < 0 && !gx_size && ms - gx_last_ms >= 5000) {
        gx::Request periodic; gx_queue(periodic, false); gx_last_ms = ms;
    }
    if (gx_cursor < 0) {
        if (!gx_size) return;
        gx_active = gx_pending[gx_head]; gx_head = (gx_head + 1) % 4; --gx_size;
        gx_cursor = 0;
        if (gx_active.request.keepalive && gx_active.request.suppress) gx_cursor = gx::topic_count();
    }
    gx_batch_at = ms;
    // Refresh each small batch, including after backpressure or reconnection.
    listener_state(gx_snapshot.bus);
    gx_snapshot.utc = time(nullptr);
    if (gx_snapshot.utc < 1767225600) gx_snapshot.utc = 0;
    gx_snapshot.now = esp_timer_get_time();
    char topic[160], payload[384];
    for (int budget = 0; budget < 2 && gx_cursor >= 0; ++budget) {
        while (gx_cursor < gx::topic_count() && !gx::matches(gx_active.request, gx_cursor)) ++gx_cursor;
        bool end = gx_cursor == gx::topic_count();
        if (end && !gx_active.respond) { gx_cursor = -1; break; }
        bool ok = end ? gx::completion(gx_id, gx_active.request, gx_snapshot.utc, topic, sizeof topic, payload, sizeof payload) :
                        gx::notification(gx_cursor, gx_id, gx_snapshot, topic, sizeof topic, payload, sizeof payload);
        // This ESP-IDF MQTT version marks the next queued packet with ID 0 as
        // transmitted after deleting a QoS-0 packet. Bursts of queued QoS-0
        // notifications therefore strand alternate packets until expiry.
        // QoS 1 gives every notification a distinct ID and broker acknowledgement.
        if (ok && esp_mqtt_client_enqueue(client, topic, payload, strlen(payload), 1, 0, true) < 0) return;
        if (ok) ++gx_published;
        if (end) gx_cursor = -1; else ++gx_cursor;
    }
}

void set_error(const char *text) {
    portENTER_CRITICAL(&error_lock);
    snprintf(last_error, sizeof last_error, "%s", text);
    portEXIT_CRITICAL(&error_lock);
}

// In the client's task.
void on_event(void *, esp_event_base_t, int32_t id, void *data) {
    auto *e = static_cast<esp_mqtt_event_handle_t>(data);
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        set_error("");
        esp_mqtt_client_publish(e->client, status_topic, "online", 0, 1, 1);
        esp_mqtt_client_subscribe(e->client, command_filter, 1);
        esp_mqtt_client_subscribe(e->client, ha_status_topic, 1);
        esp_mqtt_client_subscribe(e->client, gx_read_filter, 0);
        esp_mqtt_client_subscribe(e->client, rpc_request_topic, 0);
        announce = true;  // the discovery messages go out from the Arduino loop
        up = true;
        break;
    case MQTT_EVENT_DISCONNECTED:
        up = false;
        break;
    case MQTT_EVENT_DATA: {
        Message m;
        bool whole = e->current_data_offset == 0 && e->data_len == e->total_data_len;
        if (!whole || e->topic_len <= 0 || e->topic_len >= (int)sizeof m.topic ||
            e->data_len >= (int)sizeof m.payload || memchr(e->data, 0, e->data_len)) {
            break;  // nothing we listen to is that long
        }
        memcpy(m.topic, e->topic, e->topic_len);
        m.topic[e->topic_len] = 0;
        memcpy(m.payload, e->data, e->data_len);
        m.payload[e->data_len] = 0;
        m.retained = e->retain;
        m.received_at = esp_timer_get_time();
        xQueueSend(inbox, &m, 0);
        break;
    }
    case MQTT_EVENT_ERROR: {
        char text[112];
        const esp_mqtt_error_codes_t *err = e->error_handle;
        if (err && err->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            int code = err->connect_return_code;
            snprintf(text, sizeof text, "the broker refused the connection: %s (code %d)",
                     code == MQTT_CONNECTION_REFUSE_BAD_USERNAME || code == MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED
                         ? "user name or password not accepted"
                         : code == MQTT_CONNECTION_REFUSE_ID_REJECTED ? "client id rejected" : "refused",
                     code);
        } else if (err && err->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            snprintf(text, sizeof text, "no connection to %s:%u (socket error %d)", cfg.host, cfg.port,
                     err->esp_transport_sock_errno);
        } else {
            snprintf(text, sizeof text, "MQTT error");
        }
        set_error(text);
        break;
    }
    default:
        break;
    }
}

void start() {
    if (!cfg.enabled || !cfg.host[0]) return;
    snprintf(status_topic, sizeof status_topic, "%s/status", cfg.base);
    snprintf(state_topic, sizeof state_topic, "%s/state", cfg.base);
    snprintf(command_filter, sizeof command_filter, "%s/+/set", cfg.base);
    snprintf(ha_status_topic, sizeof ha_status_topic, "%s/status", cfg.prefix);
    snprintf(gx_read_filter, sizeof gx_read_filter, "R/%s/#", gx_id.installation);
    snprintf(rpc_request_topic, sizeof rpc_request_topic, "brain_zero/%s/request", gx_id.installation);
    esp_mqtt_client_config_t c = {};
    c.host = cfg.host;
    c.port = cfg.port;
    c.client_id = ids.node;
    c.username = cfg.user[0] ? cfg.user : nullptr;
    c.password = cfg.password[0] ? cfg.password : nullptr;
    c.lwt_topic = status_topic;  // Home Assistant shows every entity unavailable when we drop off
    c.lwt_msg = "offline";
    c.lwt_qos = 1;
    c.lwt_retain = 1;
    c.keepalive = 30;
    c.buffer_size = 1536;  // the largest discovery message is about 600 bytes
    c.out_buffer_size = 1536;
    c.network_timeout_ms = 5000;
    c.reconnect_timeout_ms = 10000;
    c.task_stack = 6144;
    c.transport = MQTT_TRANSPORT_OVER_TCP;
    client = esp_mqtt_client_init(&c);
    if (!client) {
        set_error("could not start the MQTT client");
        return;
    }
    esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, on_event, nullptr);
    if (esp_mqtt_client_start(client) != ESP_OK) {
        esp_mqtt_client_destroy(client);
        client = nullptr;
        set_error("could not start the MQTT client");
    }
}

void stop() {
    if (!client) return;
    if (up) esp_mqtt_client_publish(client, status_topic, "offline", 0, 1, 1);
    esp_mqtt_client_stop(client);
    esp_mqtt_client_destroy(client);
    client = nullptr;
    up = false;
    announce = false;
    discovery_cursor = -1;
    wifi_ready_at = 0;
    gx_cursor = -1; gx_head = gx_size = 0;
    set_error("");
}

// Send retained discovery gradually so reconnecting does not allocate every
// entity's message at once. With discovery off, send the same tombstones.
void publish_discovery(uint32_t now) {
    if (discovery_cursor < 0 || now - discovery_at < DISCOVERY_GAP_MS) return;
    discovery_at = now;
    static char topic[160], payload[1024];
    if (ha::discovery(discovery_cursor, cfg, ids, !cfg.discovery, topic, sizeof topic, payload, sizeof payload)) {
        if (esp_mqtt_client_enqueue(client, topic, payload, strlen(payload), 1, 1, true) < 0) return;
        ++published;
    }
    if (++discovery_cursor >= ha::entity_count()) discovery_cursor = -1;
}

bool publish_state() {
    static vebus::State st;
    static web::EssView v;
    static char json[2048];
    listener_state(st);
    listener_ess(v);
    ha::Extra x;
    x.rssi = WiFi.RSSI();
    x.uptime_s = millis() / 1000;
    size_t n = ha::state_json(st, v, esp_timer_get_time(), x, json, sizeof json);
    if (!n || esp_mqtt_client_enqueue(client, state_topic, json, n, 0, 0, true) < 0) return false;
    ++published;
    return true;
}

bool request_id(const char *id) {
    return id && strlen(id) == 32 && strspn(id, "0123456789abcdef") == 32;
}

bool valid_fields(const cJSON *fields) {
    if (!cJSON_IsObject(fields) || cJSON_GetArraySize(fields) > 40) return false;
    const cJSON *field = nullptr;
    cJSON_ArrayForEach(field, fields) {
        if (!field->string || !*field->string || strlen(field->string) > 32 ||
            !cJSON_IsString(field) || strlen(field->valuestring) > 128 ||
            cJSON_GetObjectItemCaseSensitive(fields, field->string) != field) return false;
    }
    return true;
}

void controller_rpc(const Message &m) {
    const uint32_t now_ms = millis();
    if (m.retained || !up || !client || now_ms - last_rpc_ms < 100 ||
        esp_timer_get_time() - m.received_at > 3000000 ||
        esp_mqtt_client_get_outbox_size(client) >= OUTBOX_LIMIT) return;
    last_rpc_ms = now_ms;
    cJSON *root = cJSON_ParseWithLengthOpts(m.payload, strlen(m.payload) + 1, nullptr, 1);
    const auto *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    if (!cJSON_IsString(id) || !request_id(id->valuestring)) {
        cJSON_Delete(root);
        return;
    }
    const auto *path = cJSON_GetObjectItemCaseSensitive(root, "path");
    const auto *op = cJSON_GetObjectItemCaseSensitive(root, "op");
    const auto *fields = cJSON_GetObjectItemCaseSensitive(root, "fields");
    const auto *expiry = cJSON_GetObjectItemCaseSensitive(root, "expires");
    const bool write = cJSON_IsString(op) && !strcmp(op->valuestring, "set");
    const bool read = cJSON_IsString(op) && !strcmp(op->valuestring, "get");
    const auto now = time(nullptr);
    const char *response;
    if (!(read || write) || !cJSON_IsString(path) || !valid_fields(fields) ||
        (read && fields->child)) {
        response = "{\"status\":400,\"data\":{\"error\":\"Invalid controller request\"}}";
    } else if (write && (now < 1767225600 || !cJSON_IsNumber(expiry) ||
               expiry->valuedouble <= now || expiry->valuedouble > double(now) + 10)) {
        response = "{\"status\":409,\"data\":{\"error\":\"Command expired or controller clock is not ready\"}}";
    } else {
        if (write) {
            for (const auto &previous : recent_write_ids) {
                if (!strcmp(previous, id->valuestring)) {
                    cJSON_Delete(root);
                    return;
                }
            }
            memcpy(recent_write_ids[next_write_id], id->valuestring, 33);
            next_write_id = (next_write_id + 1) % 16;
        }
        response = web::controller_request(path->valuestring, write, fields);
    }
    char topic[80];
    snprintf(topic, sizeof topic, "brain_zero/%s/reply/%s", gx_id.installation, id->valuestring);
    cJSON_Delete(root);
    // QoS 1 acknowledges the response to the controller and gives it a distinct outbox ID.
    esp_mqtt_client_enqueue(client, topic, response, strlen(response), 1, 0, true);
    if (write) state_now = true;
}

void handle(const Message &m) {
    if (!strcmp(m.topic, rpc_request_topic)) {
        controller_rpc(m);
        return;
    }
    gx::Request request;
    if (!strncmp(m.topic, "R/", 2)) {
        if (m.retained || esp_timer_get_time() - m.received_at > 3000000 ||
            !gx::read_request(gx_id, m.topic, request) ||
            (request.keepalive && !gx_options(m.payload, request)) || !gx_queue(request, true)) ++gx_refused;
        else ++gx_reads;
        return;
    }
    if (!strcmp(m.topic, ha_status_topic)) {  // Home Assistant restarted: announce again
        if (!strcmp(m.payload, "online") && discovery_cursor < 0) announce = true;
        return;
    }
    char key[32], why[120] = "";
    if (!ha::command_key(cfg, m.topic, strlen(m.topic), key, sizeof key)) return;
    commands++;
    static web::EssView v;
    listener_ess(v);
    ess::Settings s = v.settings;
    // Saved to flash within a minute rather than at once: automations may send setpoints often.
    bool ok;
    if (inverter::command_key(key)) {
        if (m.retained || esp_timer_get_time() - m.received_at > 3000000) {
            snprintf(why, sizeof why, "Retained or expired inverter commands are refused; publish a fresh command");
            ok = false;
        } else {
            ok = listener_inverter_command(key, m.payload, why, sizeof why);
        }
    } else {
        ok = ha::apply_command(key, m.payload, s, why, sizeof why) && listener_ess_set(s, why, sizeof why, false);
    }
    if (!ok) refused++;
    snprintf(last_command, sizeof last_command, "%s = %s: %s", key, m.payload, ok ? (inverter::command_key(key) ? "queued" : "done") : why);
    state_now = true;  // Home Assistant sees the result at once, and a refused value springs back
}

void json_string(char *out, size_t cap, size_t *n, const char *s) {
    auto put = [&](char c) {
        if (*n + 1 < cap) out[(*n)++] = c;
    };
    put('"');
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            put('\\');
            put((char)c);
        } else if (c >= 0x20 && c < 0x7F) {
            put((char)c);
        }
    }
    put('"');
    out[*n < cap ? *n : cap - 1] = 0;
}

}  // namespace

void begin(const ha::Settings &s) {
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(ids.node, sizeof ids.node, "vebus_listener_%02x%02x%02x", mac[3], mac[4], mac[5]);
    snprintf(ids.version, sizeof ids.version, "built %s %s", __DATE__, __TIME__);
    gx::identity(mac, ids.version, gx_id);
    inbox = xQueueCreate(8, sizeof(Message));
    cfg = s;
}

void apply(const ha::Settings &s) {
    stop();
    cfg = s;
    wifi_ready_at = 0;
}

void loop() {
    if (!inbox) return;
    const uint32_t ms = millis();
    if (!client && cfg.enabled && cfg.host[0]) {
        if (WiFi.status() != WL_CONNECTED || !uint32_t(WiFi.localIP())) wifi_ready_at = 0;
        else if (!wifi_ready_at) wifi_ready_at = ms;
        else if (ms - wifi_ready_at >= WIFI_SETTLE_MS) { wifi_ready_at = ms; start(); }
    }
    Message m;
    for (int budget = 0; budget < 8 && xQueueReceive(inbox, &m, 0); ++budget) handle(m);
    if (!client || !up) return;
    if (announce.exchange(false)) {
        discovery_cursor = 0;
        discovery_at = ms - DISCOVERY_GAP_MS;
        snprintf(ids.url, sizeof ids.url, "http://%s/", WiFi.localIP().toString().c_str());
        state_now = true;
        gx_last_ms = ms - 5000;
    }
    if (esp_mqtt_client_get_outbox_size(client) >= OUTBOX_LIMIT) return;
    if ((state_now || ms - last_state_ms >= cfg.interval_s * 1000u) && ms - state_attempt_ms >= GX_GAP_MS) {
        state_attempt_ms = ms;
        if (publish_state()) { state_now = false; last_state_ms = ms; return; }
    }
    publish_discovery(ms);
    if (esp_mqtt_client_get_outbox_size(client) >= OUTBOX_LIMIT) return;
    gx_publish();
}

size_t status_json(char *out, size_t cap) {
    char error[112];
    portENTER_CRITICAL(&error_lock);
    memcpy(error, last_error, sizeof error);
    portEXIT_CRITICAL(&error_lock);
    const char *state = !cfg.enabled ? "off" : !cfg.host[0] ? "no broker set" : up ? "connected"
                                                               : error[0]          ? "not connected"
                                                                                   : "connecting";
    int k = snprintf(out, cap,
                     "{\"state\":\"%s\",\"broker\":\"%s:%u\",\"node\":\"%s\",\"state_topic\":\"%s/state\","
                     "\"status_topic\":\"%s/status\",\"command_topic\":\"%s/<key>/set\",\"published\":%lu,"
                     "\"commands\":%lu,\"refused\":%lu,\"discovery_remaining\":%d,\"error\":",
                     state, cfg.host, cfg.port, ids.node, cfg.base, cfg.base, cfg.base, (unsigned long)published,
                     (unsigned long)commands, (unsigned long)refused,
                     discovery_cursor < 0 ? 0 : ha::entity_count() - discovery_cursor);
    if (k < 0 || (size_t)k >= cap) return 0;
    size_t n = (size_t)k;
    json_string(out, cap, &n, up ? "" : error);
    const char *mid = ",\"last_command\":";
    for (const char *c = mid; *c && n + 1 < cap; c++) out[n++] = *c;
    json_string(out, cap, &n, last_command);
    if (n < cap) {
        int extra = snprintf(out + n, cap - n,
            ",\"victron_gx\":{\"installation_id\":\"%s\",\"read_only\":true,\"sensors\":%d,\"published\":%lu,\"read_requests\":%lu,\"refused_requests\":%lu,\"outbox_bytes\":%d}",
            gx_id.installation, gx::sensor_count(), (unsigned long)gx_published, (unsigned long)gx_reads, (unsigned long)gx_refused,
            client ? esp_mqtt_client_get_outbox_size(client) : 0);
        if (extra < 0 || size_t(extra) >= cap - n) return 0;
        n += extra;
    }
    if (n + 1 < cap) out[n++] = '}';
    out[n < cap ? n : cap - 1] = 0;
    return n;
}

}  // namespace mqtt_link
