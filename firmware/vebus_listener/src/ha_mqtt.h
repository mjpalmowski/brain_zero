// Home Assistant over MQTT: the topics, the discovery messages that make the listener's readings and
// ESS settings appear in Home Assistant as one device, the state message they read, and the commands
// they send back. Plain C++ (no Arduino), so test/ha_mqtt_test.cpp can run it on a PC; mqtt_link.cpp
// does the networking with ESP-IDF's MQTT client.
//
// Topics, with <base> and <prefix> from the settings:
//   <base>/status                          online or offline (retained; offline is the last will)
//   <base>/state                           every reading and setting as one JSON object
//   <base>/<key>/set                       a command, e.g. vebus_listener/grid_setpoint/set 50
//   <prefix>/<component>/<node>/<key>/config   one discovery message per entity, retained
// Grid power and the setpoints count + = import, as Home Assistant and Venus's menu do; on the bus the
// setpoint is + = export. Commands change the ESS settings as the page does, within the same checks;
// the safety cap is not among them and stays on the page.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ess_policy.h"
#include "vebus_state.h"
#include "web.h"

namespace ha {

// The MQTT settings, kept in flash.
struct Settings {
    uint16_t version = 1;
    bool enabled = false;
    char host[64] = "";
    uint16_t port = 1883;
    char user[48] = "";
    char password[64] = "";        // never sent back by the API
    char base[48] = "vebus_listener";
    bool discovery = true;
    char prefix[32] = "homeassistant";
    uint16_t interval_s = 5;       // state message period
};

bool validate(const Settings &s, char *why, size_t cap);

// Who this listener is, for the discovery messages.
struct Ids {
    char node[32] = "vebus_listener";  // unique per board: vebus_listener_<last three MAC bytes>
    char version[32] = "";             // firmware build
    char url[48] = "";                 // the dashboard, http://<address>/
};

int entity_count();
// Discovery message i: its topic and JSON (or an empty payload with `remove`, which deletes the entity
// from Home Assistant). False past the last entity, or if a buffer is too small.
bool discovery(int i, const Settings &s, const Ids &ids, bool remove, char *topic, size_t tcap, char *payload,
               size_t pcap);

// Everything the entities read, as one JSON object for <base>/state. `now` is the listener's clock (µs),
// for the readings' ages: anything stale goes out as null, which Home Assistant shows as unknown.
struct Extra {
    int rssi = 0;
    uint32_t uptime_s = 0;
};
size_t state_json(const vebus::State &st, const web::EssView &v, int64_t now, const Extra &x, char *out, size_t cap);

// The <key> of a command topic "<base>/<key>/set", or false.
bool command_key(const Settings &s, const char *topic, size_t len, char *key, size_t cap);
// A command applied to a copy of the ESS settings: false, with the reason, for an unknown key or a
// value out of range. The caller checks the whole settings again before they take effect.
bool apply_command(const char *key, const char *payload, ess::Settings &s, char *why, size_t cap);

}  // namespace ha
