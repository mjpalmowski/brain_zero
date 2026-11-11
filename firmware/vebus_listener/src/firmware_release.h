#pragma once

#include <stddef.h>
#include <stdint.h>

namespace firmware_release {
struct Build {
    char tag[24] = {};
    uint32_t version = 0, size = 0;
    uint8_t sha256[32] = {};
    bool preview = false;
};

// An empty tag means that GitHub has no newer image for this controller.
// These calls perform bounded network I/O and belong on the update worker.
uint32_t tag_version(const char *tag);
const char *check(uint32_t running, size_t capacity, Build &out);
using Sink = const char *(*)(const uint8_t *, size_t, void *);
const char *download(const Build &, Sink, void *);
}
