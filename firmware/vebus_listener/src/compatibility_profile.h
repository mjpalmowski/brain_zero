#pragma once

#include "requester.h"

namespace compatibility_profile {

struct Profile {
    uint32_t firmware = 0;
    uint16_t grid_bytes = 0;
    uint16_t assistant_bytes = 0;
    uint8_t nominal_voltage = 0;
    uint8_t descriptor_sha256[32] = {};
};

// Validate a complete settings read and compute the fingerprint before sending the response.
bool prepare(const vebus::Requester::SettingsRead &read, Profile &profile, const char **why);

using Write = bool (*)(const char *data, size_t len, void *context);

// Stream the public format used by tools/export_compatibility.py in the GitHub repo.
// Pass the same snapshot used by prepare(). A failed write stops the stream.
bool write_json(const vebus::Requester::SettingsRead &read, const Profile &profile, Write write, void *context);

}  // namespace compatibility_profile
