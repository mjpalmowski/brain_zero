// The MQTT connection, through ESP-IDF's MQTT client, which connects and reconnects in a task of its
// own so a slow or missing broker never holds up the pages. Once connected it announces the listener
// to Home Assistant (ha_mqtt.h), publishes the state every few seconds, and hands Home Assistant's
// commands to the ESS settings as the page does. The settings live in flash (main.cpp) and are set on
// the MQTT page (web.h).
#pragma once

#include <stddef.h>

#include "ha_mqtt.h"

namespace mqtt_link {

void begin(const ha::Settings &s);          // from setup(), after Wi-Fi has been started
void apply(const ha::Settings &s);          // new settings from the page: reconnect with them
void loop();                                // often, from the Arduino loop
size_t status_json(char *out, size_t cap);  // {"state":"connected",...} for the pages

}  // namespace mqtt_link
