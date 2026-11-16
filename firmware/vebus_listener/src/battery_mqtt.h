#pragma once
#include <stddef.h>
#include "battery_bms.h"
namespace ha { struct Settings; struct Ids; }
namespace battery_mqtt {
// Independent per-pack topics keep payloads bounded and carry null for fields
// whose own source frames have expired. No battery commands are subscribed.
const char *peer_name(unsigned peer);
size_t state_json(const battery_bms::Snapshot &snapshot, char *out, size_t cap);
int entity_count();
bool discovery(int index, const ha::Settings &settings, const ha::Ids &ids,
               bool remove, char *topic, size_t topic_cap, char *out, size_t cap);
}
