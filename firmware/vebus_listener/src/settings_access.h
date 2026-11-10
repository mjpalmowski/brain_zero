// VEConfigure's embedded access credentials. No password generation or stored installer key.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace vebus {

inline void erase_secret(void *p, size_t n) {
    volatile uint8_t *b = static_cast<volatile uint8_t *>(p);
    while (n--) *b++ = 0;
}

struct SettingsAccess {
    static constexpr unsigned MAX = 7, BYTES = 8;
    unsigned count = 0;
    uint8_t bytes[MAX][BYTES] = {};
    int64_t valid_time = 0, settings_time = 0;

    void erase_bytes() { erase_secret(bytes, sizeof bytes); }
    void clear() { erase_bytes(); count = 0; valid_time = settings_time = 0; }

    // Native mk2vsc 1.32's ordinary upload path: an unsigned settings age <= one day,
    // and validTime within one day either side of now. Require the S3's clock to be set.
    const char *problem(int64_t utc) const {
        if (count > MAX) return "too many embedded access credentials";
        if (!count) return nullptr;
        if (utc < 1767225600 || utc > INT64_MAX - 172800)
            return "set the brain_zero clock before applying embedded authorization";
        if (settings_time < utc - 86400 || settings_time > utc ||
            valid_time < utc - 86400 || valid_time > utc + 86400)
            return "embedded authorization expired: reopen and save the file in VEConfigure";
        return nullptr;
    }
    int64_t expires() const {
        int64_t t = valid_time < settings_time ? valid_time : settings_time;
        return t + 86400;  // used only after problem() succeeds
    }
};

}  // namespace vebus
