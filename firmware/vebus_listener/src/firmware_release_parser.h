#pragma once

#include "firmware_release.h"

namespace firmware_release {

inline const char *asset_name(bool site) { return site ? "firmware-site.bin" : "firmware.bin"; }

// next returns an unsigned byte, -1 at EOF, or another negative value on I/O error.
bool parse_release_list(int (*next)(void *), void *context, uint32_t running, size_t capacity,
                        bool site, Build &out);

}
