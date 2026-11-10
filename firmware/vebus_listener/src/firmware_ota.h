#pragma once
#include <stddef.h>
#include <stdint.h>

namespace firmware_ota {
constexpr size_t PREFIX_BYTES = 36; // image header, first segment header, application descriptor magic

inline bool app_prefix(const uint8_t *p) {
    // ESP32-S3 chip ID 9 and ESP-IDF application descriptor 0xabcd5432.
    return p[0] == 0xe9 && p[1] >= 1 && p[1] <= 16 && p[12] == 9 && p[13] == 0 &&
           p[32] == 0x32 && p[33] == 0x54 && p[34] == 0xcd && p[35] == 0xab;
}

enum class Action { NONE, INSTALL, ROLLBACK };
struct Status {
    bool trial, staged;
    size_t max_bytes, staged_bytes;
    unsigned running_offset;
    const char *slot, *target, *token, *message;
    Action queued;
};

void begin();
Status status();
bool authorized(const char *token);
const char *start(size_t bytes);
const char *write(const uint8_t *data, size_t bytes);
const char *finish();
void discard(const char *message);
const char *keep();
const char *queue(Action action);
void cancel_restart(const char *message);
bool activate(); // selects the staged image for boot; main loop restarts only after success
}
