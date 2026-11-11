#pragma once

#include <time.h>

namespace controller_time {

struct Zone;

// Returns an immutable entry for an IANA identifier or alias, or nullptr.
const Zone *find(const char *name);
const char *name(const Zone &zone);
const char *names_json();

// Converts UTC to local calendar fields, including weekday and DST state.
// Returns false when the bundled data or time_t cannot cover the timestamp.
bool local(const Zone &zone, time_t utc, struct tm &out);

}  // namespace controller_time
