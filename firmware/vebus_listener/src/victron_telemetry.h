#pragma once
#include <stddef.h>
#include <stdint.h>
#include "vebus_state.h"

// A deliberately read-only subset of Venus's N/R MQTT interface. No W topics
// or settings are described here, so Victron GX creates sensors only.
namespace gx {
struct Identity { char installation[13] = ""; char firmware[48] = ""; };
struct Snapshot {
    vebus::State bus;
    bool pace_valid = false;
    float pace_soc = 0, pace_voltage = 0;
    uint32_t utc = 0;
    int64_t now = 0;
};
struct Request {
    char path[112] = "";  // relative path, or empty for a complete refresh
    char echo[129] = "";
    bool keepalive = false, suppress = false;
};
void identity(const uint8_t mac[6], const char *firmware, Identity &out);
int topic_count();
int sensor_count();
// Refuses writes, other installations, wildcards and overlong paths.
bool read_request(const Identity &id, const char *topic, Request &out);
bool matches(const Request &request, int index);
// Fixed topic whitelist; unavailable readings are JSON null, never stale zeros.
bool notification(int index, const Identity &id, const Snapshot &snapshot,
                  char *topic, size_t topic_cap, char *payload, size_t payload_cap);
bool completion(const Identity &id, const Request &request, uint32_t utc,
                char *topic, size_t topic_cap, char *payload, size_t payload_cap);
} // namespace gx
