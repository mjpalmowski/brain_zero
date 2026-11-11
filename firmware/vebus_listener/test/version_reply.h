#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

inline size_t version_reply(uint8_t *out, uint8_t id, uint32_t firmware = 2623563) {
    const uint8_t version[] = {0x00, id, 0x82, uint8_t(firmware), uint8_t(firmware >> 8),
                               uint8_t(firmware >> 16), uint8_t(firmware >> 24),
                               0x08, 0x1D, 0x00, 0x00, 0x43, 0x10};
    memcpy(out, version, sizeof version);
    return sizeof version;
}
