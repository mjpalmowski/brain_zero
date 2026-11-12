// Synthetic codec regressions. No captured exports, device identities, serials,
// credentials, serial ports or network connections are used by this test.
#include <stdio.h>
#include <string.h>

#include <vector>

#include "rvsc.h"

using vebus::Requester;
using Bytes = std::vector<uint8_t>;
using Read = Requester::SettingsRead;
static int failures = 0;
#define CHECK(condition) do { if (!(condition)) { \
    printf("FAIL line %d: %s\n", __LINE__, #condition); ++failures; } } while (0)

static void u16(Bytes &b, unsigned value) {
    b.push_back(uint8_t(value)); b.push_back(uint8_t(value >> 8));
}
static void u32(Bytes &b, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) b.push_back(uint8_t(value >> (i * 8)));
}
static uint32_t word(const Bytes &b, size_t at) {
    return uint32_t(b[at]) | uint32_t(b[at + 1]) << 8 |
           uint32_t(b[at + 2]) << 16 | uint32_t(b[at + 3]) << 24;
}
static uint32_t sum(const Bytes &b, size_t start, size_t end) {
    uint32_t value = 0;
    for (size_t at = start; at < end; ++at) value += uint32_t(b[at]) << ((at - start) % 4 * 8);
    return value;
}
static void section(Bytes &file, const char *name, const Bytes &body) {
    const size_t start = file.size();
    u16(file, unsigned(strlen(name)));
    file.insert(file.end(), name, name + strlen(name));
    u32(file, uint32_t(file.size() + 8 + body.size()));
    file.insert(file.end(), body.begin(), body.end());
    u32(file, sum(file, start, file.size()));
}
static size_t body_at(const Bytes &file, const char *name, size_t *start = nullptr, size_t *end = nullptr) {
    for (size_t at = 31; at + 2 < file.size();) {
        const size_t len = file[at] | file[at + 1] << 8;
        const size_t next = word(file, at + 2 + len);
        if (strlen(name) == len && !memcmp(file.data() + at + 2, name, len)) {
            if (start) *start = at;
            if (end) *end = next;
            return at + 2 + len + 4;
        }
        at = next;
    }
    return 0;
}
static void repair(Bytes &file, const char *name) {
    size_t start = 0, end = 0;
    CHECK(body_at(file, name, &start, &end) != 0);
    const uint32_t checksum = sum(file, start, end - 4);
    for (unsigned i = 0; i < 4; ++i) file[end - 4 + i] = uint8_t(checksum >> (i * 8));
}

// Independent schema-2/unit-2 fixture, including its one-byte unit marker.
// The eight-byte legacy signature is observed firmware metadata; all settings,
// identity and Assistant bytes here are deliberately synthetic.
static Bytes legacy_file(unsigned version_bytes, bool producer = false, bool extra_grid = false,
                         unsigned assistant_fill = 61) {
    Bytes file, schema, unit;
    u16(file, 29);
    const char magic[] = "VEConfig setting section file";
    file.insert(file.end(), magic, magic + 29);
    if (producer) {
        Bytes info; u32(info, 1); u16(info, 4);
        const char text[] = "1.33"; info.insert(info.end(), text, text + 4);
        section(file, "Mk2vscInfo", info);
    }
    u32(schema, 2); u32(schema, 2628459); schema.push_back(47); u16(schema, 1920);
    for (unsigned i = 0; i < 192; ++i) {
        const unsigned fields[] = {uint16_t(-100), 0, 5760, 4800, 6400};
        for (unsigned value : fields) u16(schema, i == 2 ? value : 0);
    }
    u16(schema, 80); schema.resize(schema.size() + 80); schema[schema.size() - 80] = 0x80;
    u16(schema, 192); schema.resize(schema.size() + 192); schema[schema.size() - 190] = 0x81;
    if (extra_grid) u16(schema, 0);  // invalid: schema 2 has no grid-length field
    section(file, "BareSettingInfo", schema);
    u32(unit, 2);
    const uint8_t id[] = {1, 2, 3, 4}; unit.insert(unit.end(), id, id + 4);
    u32(unit, 2628459); unit.push_back(1);
    const uint8_t version[] = {0x6B, 0x1B, 0x28, 0, 8, 0x0D, 0, 0, 0xA5, 0x5A};
    unit.insert(unit.end(), version, version + 4); u16(unit, version_bytes - 4);
    unit.insert(unit.end(), version + 4, version + version_bytes);
    const uint8_t info[] = {47, 1, 1, 1, 0, 0, 0, 0, 0xF0, 0};
    unit.insert(unit.end(), info, info + 10);
    const char serial[] = "BZSIM000000"; u16(unit, sizeof serial - 1);
    unit.insert(unit.end(), serial, serial + sizeof serial - 1);
    u16(unit, 0); u32(unit, 0); u32(unit, 0); u32(unit, 1234); u32(unit, 0);
    u16(unit, 384);
    for (unsigned i = 0; i < 192; ++i) u16(unit, i == 2 ? 5600 : 0);
    u16(unit, 3); unit.push_back(0xAB); unit.push_back(0xCD); unit.push_back(0xEF);
    unit.push_back(0xFF); u16(unit, assistant_fill);
    section(file, "BareSettingData", unit);
    return file;
}

static void snapshot(Read &r, bool legacy, unsigned version_bytes = 10) {
    r = Read(); r.done = true; r.firmware = legacy ? 2628459 : 2623563;
    snprintf(r.serial, sizeof r.serial, "BZSIM000000");
    const uint8_t version[] = {0x6B, 0x1B, 0x28, 0, 8, 0x0D, 0, 0, 0xA5, 0x5A};
    memcpy(r.version, version, 10);
    if (!legacy) { r.version[0] = 0x4B; r.version[1] = 8; r.version[5] = 0x1D; }
    r.version_bytes = uint8_t(version_bytes); r.version_ok = version_bytes >= 10;
    r.unit_ok = true;
    const uint8_t id[] = {1, 2, 3, 4}, info[] = {47, 1, 1, 1, 0, 0, 0, 0, 0xF0, 0};
    memcpy(r.unit_id, id, 4); memcpy(r.unit_info, info, 10);
    memset(r.block_got, 3, sizeof r.block_got); r.block[0][0] = 0x80;
    for (auto &setting : r.s) setting.state = Requester::UNSUPPORTED;
    auto &setting = r.s[2]; setting.state = Requester::READ_OK;
    setting.scale = -100; setting.def = 5760; setting.min = 4800; setting.max = 6400;
    setting.value = 5600; setting.access = 0x81;
    r.grid_availability = legacy ? Requester::GridAvailability::LEGACY_OMITTED
                                 : Requester::GridAvailability::AVAILABLE;
    r.grid.ended = !legacy;
    r.assistants.ended = true; r.assistants.count = 1; r.assistants.len[0] = 3;
    r.assistants.data[0][0] = 0xAB; r.assistants.data[0][1] = 0xCD; r.assistants.data[0][2] = 0xEF;
}

static bool parse(const Bytes &file, rvsc::File &out) {
    char why[180];
    return rvsc::parse(file.data(), file.size(), out, why, sizeof why);
}

int main() {
    static Read read;
    static rvsc::File parsed;
    static uint8_t output[rvsc::MAX_SIZE];
    const char *why = nullptr;
    for (unsigned version_bytes : {8u, 10u}) {
        const Bytes expected = legacy_file(version_bytes);
        CHECK(parse(expected, parsed));
        CHECK(parsed.schema_version == 2 && parsed.unit_version == 2 && !parsed.grid_present && !parsed.grid_len);
        CHECK(parsed.version_bytes == version_bytes && parsed.unit_const[0] == 1 && parsed.settings[2] == 5600);
        CHECK(parsed.access[2] == 0x81 && parsed.block[0] == 0x80 && parsed.assistant_bytes == 64);
        CHECK(parsed.assistants[2] == 0xEF && parsed.assistants[63] == 0xFF);
        CHECK(version_bytes == 8 ? parsed.version[8] == 0 && parsed.version[9] == 0
                                : parsed.version[8] == 0xA5 && parsed.version[9] == 0x5A);
        snapshot(read, true, version_bytes);
        CHECK(rvsc::ready(read, &why));
        const size_t size = rvsc::build(read, 1234, output, sizeof output);
        CHECK(size == expected.size() && !memcmp(output, expected.data(), size));
        static rvsc::Check check;
        CHECK(!rvsc::check(parsed, read, check) && !check.ok && strstr(check.why, "read-only"));
        CHECK(check.plan.settings == 0 && !check.plan.assistants);
        parsed.credentials.count = 1; memset(parsed.credentials.bytes, 0xA5, sizeof parsed.credentials.bytes);
        CHECK(!rvsc::check(parsed, read, check) && check.plan.settings == 0);
        CHECK(parse(legacy_file(version_bytes, true), parsed) && !strcmp(parsed.mk2vsc, "1.33"));
    }

    // A modern present-empty grid remains distinct from legacy omission.
    snapshot(read, false);
    size_t size = rvsc::build(read, 1234, output, sizeof output);
    CHECK(size && parse(Bytes(output, output + size), parsed));
    CHECK(parsed.schema_version == 4 && parsed.unit_version == 3 && parsed.version_bytes == 10);
    CHECK(parsed.grid_present && parsed.grid_len == 0 && parsed.unit_const[0] == 0xFF);
    static rvsc::Check modern_check;
    CHECK(rvsc::check(parsed, read, modern_check) && modern_check.ok && modern_check.plan.settings == 0);
    read.grid.count = 1; read.grid.len[0] = 3;
    read.grid.data[0][0] = 0x12; read.grid.data[0][1] = 0x34; read.grid.data[0][2] = 0x56;
    size = rvsc::build(read, 1234, output, sizeof output);
    Bytes modern(output, output + size);
    CHECK(size && parse(modern, parsed) && parsed.grid_present && parsed.grid_len == 3 && parsed.grid[2] == 0x56);

    Bytes broken = legacy_file(8); broken.back() ^= 1; CHECK(!parse(broken, parsed));
    broken = legacy_file(8); broken.pop_back(); CHECK(!parse(broken, parsed));
    CHECK(!parse(legacy_file(8, false, true), parsed));
    CHECK(!parse(legacy_file(8, false, false, 4097), parsed));
    broken = legacy_file(8);
    broken[body_at(broken, "BareSettingData") + 17] = 5;
    repair(broken, "BareSettingData"); CHECK(!parse(broken, parsed));  // nine stored version bytes
    broken = legacy_file(8);
    size_t unit_start = 0, unit_end = 0;
    const size_t unit_body = body_at(broken, "BareSettingData", &unit_start, &unit_end);
    const Bytes duplicate(broken.begin() + unit_body, broken.begin() + unit_end - 4);
    section(broken, "BareSettingData", duplicate); CHECK(!parse(broken, parsed));
    broken = modern;
    const size_t grid_length = body_at(broken, "BareSettingInfo") + 2207;
    broken[grid_length] = 1; broken[grid_length + 1] = 12;  // 3073 > the 3072-byte bound
    repair(broken, "BareSettingInfo"); CHECK(!parse(broken, parsed));

    for (unsigned mutation = 0; mutation < 8; ++mutation) {
        snapshot(read, true, 8);
        switch (mutation) {
        case 0: read.firmware++; break;
        case 1: read.unit_info[0]++; break;
        case 2: read.version[5] ^= 1; break;
        case 3: read.version_bytes = 9; break;
        case 4: read.version_bytes = 11; read.version_ok = true; break;
        case 5: read.grid.ended = true; break;
        case 6: read.grid_availability = Requester::GridAvailability::UNKNOWN; break;
        case 7: read.assistants.len[0] = 65; break;
        }
        CHECK(!rvsc::ready(read, &why) && !rvsc::build(read, 1234, output, sizeof output));
    }
    snapshot(read, true, 8);
    uint8_t small[64]; memset(small, 0xA5, sizeof small);
    CHECK(!rvsc::build(read, 1234, small + 1, sizeof small - 2) && small[0] == 0xA5 && small[63] == 0xA5);
    printf(failures ? "%d FAILED\n" : "all synthetic RVSC codec tests passed\n", failures);
    return failures ? 1 : 0;
}
