#include "controller_time.h"

#include <limits>
#include <stdint.h>
#include <string.h>

namespace controller_time {

struct Zone {
    const char *id;
    uint16_t rule;
};

namespace {

struct Offset {
    int16_t minutes;
    bool daylight;
};

struct Transition {
    uint32_t utc;
    Offset after;
};

struct Rule {
    uint16_t start;
    uint16_t count;
    Offset initial;
};

#include "controller_time_data.h"

}  // namespace

const Zone *find(const char *id) {
    if (!id) return nullptr;
    size_t begin = 0;
    size_t end = sizeof zones / sizeof zones[0];
    while (begin < end) {
        const size_t mid = begin + (end - begin) / 2;
        const int order = strcmp(id, zones[mid].id);
        if (order == 0) return &zones[mid];
        if (order < 0) end = mid;
        else begin = mid + 1;
    }
    return nullptr;
}

const char *name(const Zone &zone) { return zone.id; }
const char *names_json() { return zone_names_json; }

bool local(const Zone &zone, time_t utc, struct tm &out) {
    if (utc < static_cast<time_t>(FIRST_UTC) || utc > static_cast<time_t>(LAST_UTC)) return false;
    const uint32_t instant = static_cast<uint32_t>(utc);
    const Rule &rule = rules[zone.rule];
    uint16_t begin = rule.start;
    uint16_t end = rule.start + rule.count;
    while (begin < end) {
        const uint16_t mid = begin + (end - begin) / 2;
        if (instant < transitions[mid].utc) end = mid;
        else begin = mid + 1;
    }
    const Offset &offset = begin == rule.start ? rule.initial : transitions[begin - 1].after;
    const int64_t shifted = int64_t(utc) + int64_t(offset.minutes) * 60;
    if (shifted < 0 || uint64_t(shifted) > uint64_t(std::numeric_limits<time_t>::max())) return false;
    const time_t civil = static_cast<time_t>(shifted);
#ifdef _WIN32
    if (gmtime_s(&out, &civil) != 0) return false;
#else
    if (!gmtime_r(&civil, &out)) return false;
#endif
    out.tm_isdst = offset.daylight;
    return true;
}

}  // namespace controller_time
