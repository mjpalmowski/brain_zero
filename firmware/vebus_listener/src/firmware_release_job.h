#pragma once

#include "firmware_ota.h"
#include "firmware_release.h"

namespace firmware_release_job {

struct Status {
    const char *phase;
    firmware_release::Build build;
    size_t received;
    const char *message;
    bool can_stage;
    const char *blocked_by;  // first strict readiness failure; stable literal, empty on a new attempt
};

// All entry points are called from the Arduino loop. The background task owns
// OTA writes while downloading; ota_status() uses a snapshot for that interval.
bool busy();
Status status();
firmware_ota::Status ota_status();
const char *check(uint32_t running, size_t capacity);
// reserve() atomically checks readiness and defers only automatic background
// refreshes. A successful reservation is released after finish/discard, including
// every pre-launch failure. The callbacks run only on the Arduino loop task.
const char *stage(const char *tag, const char *(*reserve)(), void (*release)());
// nullptr means strictly ready. A non-null stable literal cancels immediately;
// the first failed snapshot is retained in blocked_by for diagnosis.
void poll(const char *(*not_ready)());
void discarded();

}  // namespace firmware_release_job
