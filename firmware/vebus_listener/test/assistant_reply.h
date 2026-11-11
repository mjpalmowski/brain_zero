#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

inline size_t assistant_unit_reply(uint8_t *out) {
    const uint8_t reply[] = {0x20, 0x48, 0x39, 0x1B, 0x7A, 0x2F, 1, 1, 1, 0, 0, 0, 0, 0xE0, 0};
    memcpy(out, reply, sizeof reply);
    return sizeof reply;
}

// Layout from the 2623563 startup capture. A test can supply a changing flags word.
inline uint16_t assistant_ram_word(uint8_t address, uint16_t flags) {
    switch (address) {
    case 128: return 0x0001;
    case 130: return 0x0054;
    case 132: return flags;
    case 133: return 5900;
    case 135: return 0x00A2;
    case 136: return 32766;
    default: return 0;
    }
}

inline size_t assistant_ram_reply(uint8_t *out, const uint8_t *request, size_t size, uint16_t flags) {
    out[0] = 0;
    out[1] = request[1];
    out[2] = 0x85;
    size_t reply_size = 3;
    for (size_t i = 3; i < size; ++i) {
        const uint8_t address = request[i];
        const uint16_t word = address == 13 ? 155 : assistant_ram_word(address, flags);
        out[reply_size++] = word & 0xff;
        out[reply_size++] = word >> 8;
    }
    return reply_size;
}
