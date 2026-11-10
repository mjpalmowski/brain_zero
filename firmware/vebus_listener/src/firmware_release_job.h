#pragma once

#include "firmware_ota.h"
#include "firmware_release.h"

namespace firmware_release_job {

struct Status {
    const char *phase;
    firmware_release::Build build;
    size_t received;
    const char *message;
};

// All entry points are called from the Arduino loop. The background task owns
// OTA writes while downloading; ota_status() uses a snapshot for that interval.
bool busy();
Status status();
firmware_ota::Status ota_status();
const char *check(uint32_t running, size_t capacity, bool site);
const char *stage(const char *tag);
void poll(bool (*safe_to_stage)());
void discarded();

}  // namespace firmware_release_job
