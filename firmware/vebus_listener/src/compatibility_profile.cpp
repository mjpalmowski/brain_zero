#include "compatibility_profile.h"

#include <mbedtls/sha256.h>
#include <stdio.h>
#include <string.h>

#include "multiplus_ii.h"
#include "rvsc.h"

namespace compatibility_profile {
namespace {

using vebus::Requester;
using Read = Requester::SettingsRead;

// Match the schema and unit block versions written by rvsc::build().
constexpr uint8_t SCHEMA_VERSION = 4, UNIT_VERSION = 3;
constexpr uint8_t STANDALONE[] = {1, 1, 1, 0, 0, 0, 0};
static_assert(Requester::SETTINGS == 192 && Requester::BLOCK_ARGS * Requester::BLOCK_BYTES == 80,
              "Compatibility profile format requires 192 settings and 80 flag access bytes");

void put16(uint8_t *out, uint16_t value) {
    out[0] = uint8_t(value);
    out[1] = uint8_t(value >> 8);
}

void put32(uint8_t *out, uint32_t value) {
    put16(out, uint16_t(value));
    put16(out + 2, uint16_t(value >> 16));
}

bool fail(const char **why, const char *reason) {
    if (why) *why = reason;
    return false;
}

bool valid_voltage(const Requester::Setting &s, int nominal) {
    if (s.state != Requester::READ_OK) return false;
    int64_t minimum = s.min, maximum = s.max, denominator = 1;
    if (minimum > maximum) {
        if (minimum >= 32768) minimum -= 65536;
        if (maximum >= 32768) maximum -= 65536;
    }
    if (s.scale < 0) {
        minimum += s.offset;
        maximum += s.offset;
        denominator = -int(s.scale);
    } else if (s.scale > 0) {
        minimum = (minimum + s.offset) * s.scale;
        maximum = (maximum + s.offset) * s.scale;
    }
    const int64_t expected = nominal * denominator;
    return minimum == expected && maximum > minimum && 3 * maximum <= 4 * expected;
}

bool fingerprint(const Read &r, Profile &profile) {
    // Match the exporter's little-endian layout and descriptor records. Unsupported
    // settings use zeros, as in an .rvsc backup. No saved setting values enter the hash.
    uint8_t layout[17];
    layout[0] = r.unit_info[0];
    put32(layout + 1, profile.firmware);
    put32(layout + 5, profile.firmware);
    layout[9] = SCHEMA_VERSION;
    layout[10] = UNIT_VERSION;
    put16(layout + 11, profile.grid_bytes);
    put32(layout + 13, profile.assistant_bytes);

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    bool ok = mbedtls_sha256_starts_ret(&sha, 0) == 0 &&
              mbedtls_sha256_update_ret(&sha, layout, sizeof layout) == 0 &&
              mbedtls_sha256_update_ret(&sha, r.version, sizeof r.version) == 0 &&
              mbedtls_sha256_update_ret(&sha, r.unit_info, sizeof r.unit_info) == 0 &&
              mbedtls_sha256_update_ret(&sha, reinterpret_cast<const uint8_t *>(&r.block), sizeof r.block) == 0;
    for (int i = 0; ok && i < Requester::SETTINGS; ++i) {
        const auto &s = r.s[i];
        uint8_t record[11] = {};
        if (s.state == Requester::READ_OK) {
            put16(record, uint16_t(s.scale));
            put16(record + 2, uint16_t(s.offset));
            put16(record + 4, s.def);
            put16(record + 6, s.min);
            put16(record + 8, s.max);
            record[10] = s.access;
        }
        ok = mbedtls_sha256_update_ret(&sha, record, sizeof record) == 0;
    }
    ok = ok && mbedtls_sha256_finish_ret(&sha, profile.descriptor_sha256) == 0;
    mbedtls_sha256_free(&sha);
    return ok;
}

class Output {
public:
    Output(Write write, void *context) : write_(write), context_(context) {}

    void add(const char *text) { add(text, strlen(text)); }

    void hex(const uint8_t *bytes, size_t len) {
        static const char digits[] = "0123456789abcdef";
        for (size_t i = 0; i < len; ++i) {
            const char pair[] = {digits[bytes[i] >> 4], digits[bytes[i] & 15]};
            add(pair, sizeof pair);
        }
    }

    bool flush() {
        if (ok_ && used_) ok_ = write_(buffer_, used_, context_);
        used_ = 0;
        return ok_;
    }

private:
    void add(const char *data, size_t len) {
        while (ok_ && len) {
            const size_t free = sizeof buffer_ - used_;
            const size_t count = len < free ? len : free;
            memcpy(buffer_ + used_, data, count);
            used_ += count;
            data += count;
            len -= count;
            if (used_ == sizeof buffer_) flush();
        }
    }

    Write write_;
    void *context_;
    char buffer_[512];
    size_t used_ = 0;
    bool ok_ = true;
};

}  // namespace

bool prepare(const Read &r, Profile &profile, const char **why) {
    if (!rvsc::ready(r, why)) return false;
    if (memcmp(r.unit_info + 1, STANDALONE, sizeof STANDALONE) || (r.unit_info[8] & 15) || r.unit_info[9])
        return fail(why, "Expected one standalone inverter");

    // The exporter's raw firmware and .rvsc firmware word must agree. The latter
    // uses the eighth version-reply byte as its high byte.
    if (r.version[3] != r.version[7]) return fail(why, "The settings schema and firmware reply disagree");
    const uint32_t firmware = uint32_t(r.version[0]) | uint32_t(r.version[1]) << 8 |
                              uint32_t(r.version[2]) << 16 | uint32_t(r.version[3]) << 24;
    const auto *model = multiplus_ii::find(firmware);
    if (!model) return fail(why, "Expected a recognized 230 V MultiPlus-II firmware");
    if (!valid_voltage(r.s[2], model->nominal_voltage) || !valid_voltage(r.s[3], model->nominal_voltage))
        return fail(why, "Charger descriptors disagree with the inverter battery voltage");

    if (r.grid.count <= 0 || r.grid.count > Requester::MAX_GRID_RECORDS)
        return fail(why, "Expected complete grid capability descriptors");
    unsigned grid_bytes = 0;
    for (int i = 0; i < r.grid.count; ++i) {
        if (r.grid.len[i] > Requester::RECORD) return fail(why, "Invalid grid capability descriptor length");
        grid_bytes += r.grid.len[i];
    }
    if (!grid_bytes) return fail(why, "Expected complete grid capability descriptors");
    if (r.assistants.count <= 0 || r.assistants.count > Requester::MAX_RECORDS)
        return fail(why, "Invalid VEConfigure Assistant area size");

    profile.firmware = firmware;
    profile.nominal_voltage = model->nominal_voltage;
    profile.grid_bytes = uint16_t(grid_bytes);
    profile.assistant_bytes = uint16_t(r.assistants.count * Requester::RECORD);
    if (!fingerprint(r, profile)) return fail(why, "Could not calculate the compatibility profile fingerprint");
    return true;
}

bool write_json(const Read &r, const Profile &profile, Write write, void *context) {
    if (!write) return false;
    Output out(write, context);
    char text[180];
    snprintf(text, sizeof text,
             "{\"profile\":{\"format\":1,\"model\":%u,\"unit_firmware\":%lu,\"schema_firmware\":%lu,"
             "\"descriptor_firmware\":%lu,\"nominal_voltage\":%u,\"descriptor_sha256\":\"",
             unsigned(r.unit_info[0]), (unsigned long)profile.firmware, (unsigned long)profile.firmware,
             (unsigned long)profile.firmware, unsigned(profile.nominal_voltage));
    out.add(text);
    out.hex(profile.descriptor_sha256, sizeof profile.descriptor_sha256);
    out.add("\",\"version_reply\":\"");
    out.hex(r.version, sizeof r.version);
    out.add("\",\"unit_info\":\"");
    out.hex(r.unit_info, sizeof r.unit_info);
    snprintf(text, sizeof text,
             "\",\"schema_version\":%u,\"unit_version\":%u,\"grid_bytes\":%u,\"assistant_bytes\":%u,\"flag_access\":\"",
             unsigned(SCHEMA_VERSION), unsigned(UNIT_VERSION), unsigned(profile.grid_bytes), unsigned(profile.assistant_bytes));
    out.add(text);
    out.hex(reinterpret_cast<const uint8_t *>(&r.block), sizeof r.block);
    out.add("\",\"setting_fields\":[\"scale\",\"offset\",\"default\",\"minimum\",\"maximum\",\"access\"],\"settings\":[");
    for (int i = 0; i < Requester::SETTINGS; ++i) {
        const auto &s = r.s[i];
        if (i) out.add(",");
        if (s.state != Requester::READ_OK) {
            out.add("[0,0,0,0,0,0]");
        } else {
            snprintf(text, sizeof text, "[%d,%d,%u,%u,%u,%u]", int(s.scale), int(s.offset), unsigned(s.def),
                     unsigned(s.min), unsigned(s.max), unsigned(s.access));
            out.add(text);
        }
    }
    out.add("]},\"grid\":\"");
    for (int i = 0; i < r.grid.count; ++i) out.hex(r.grid.data[i], r.grid.len[i]);
    out.add("\"}\n");
    return out.flush();
}

}  // namespace compatibility_profile
