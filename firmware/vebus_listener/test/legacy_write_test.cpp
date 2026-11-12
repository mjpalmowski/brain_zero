// Qualified legacy upload guards: synthetic settings, identity and Assistant data.
// The firmware signature alone is observed public metadata; no captured export is used.
#include <stdio.h>
#include <string.h>

#include "rvsc.h"

using vebus::Requester;
using Read = Requester::SettingsRead;
static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

static void setting(Read &r, unsigned id, int16_t scale, uint16_t def, uint16_t min,
                    uint16_t max, uint16_t current, uint8_t access = 0) {
    auto &s = r.s[id]; s.state = Requester::READ_OK; s.scale = scale;
    s.def = def; s.min = min; s.max = max; s.value = current; s.access = access;
}

static void snapshot(Read &r, unsigned bytes = 8) {
    r = Read(); r.done = true; r.firmware = 2628459;
    const uint8_t version[] = {0x6B, 0x1B, 0x28, 0, 8, 0x0D, 0, 0, 0xA5, 0x5A};
    memcpy(r.version, version, bytes); r.version_bytes = uint8_t(bytes); r.version_ok = bytes == 10;
    r.unit_ok = true; r.unit_id[0] = 1; r.unit_id[1] = 2; r.unit_id[2] = 3; r.unit_id[3] = 4;
    const uint8_t info[] = {47, 1, 1, 1, 0, 0, 0, 0, 0xF0, 0};
    memcpy(r.unit_info, info, sizeof info); snprintf(r.serial, sizeof r.serial, "BZSIM000000");
    r.started = 1000000; r.finished = 2000000;
    memset(r.block_got, 3, sizeof r.block_got);
    for (auto &s : r.s) s.state = Requester::UNSUPPORTED;
    setting(r, 0, 1, 0, 0, 7, 0);
    setting(r, 2, -100, 5760, 4800, 6400, 5600);
    setting(r, 3, -100, 5520, 4800, 6400, 5520);
    setting(r, 4, 1, 35, 0, 35, 35);
    setting(r, 10, 1, 3, 1, 3, 3, 0x80);
    setting(r, 20, 1, 0, uint16_t(-500), 500, 0);
    setting(r, 81, 1, 0, 0, 16, 0, 1);
    r.block[0][1] = 0x80;
    r.grid_availability = Requester::GridAvailability::LEGACY_OMITTED;
    r.assistants.ended = true; r.assistants.count = 64;
}

static void file(const Read &r, rvsc::File &f) {
    static uint8_t data[rvsc::MAX_SIZE]; char why[180];
    const size_t n = rvsc::build(r, 1767225800, data, sizeof data);
    CHECK(n && rvsc::parse(data, n, f, why, sizeof why));
}

int main() {
    static Read read;
    static rvsc::File f;
    static rvsc::Check result;
    const int64_t utc = 1767225800;
    snapshot(read); file(read, f);
    CHECK(rvsc::check(f, read, result, utc) && result.ok && !result.plan.settings);
    f.settings[2] = 5750;
    CHECK(rvsc::check(f, read, result, utc) && result.ok && result.plan.settings == 1);
    CHECK(result.plan.setting_id[0] == 2 && result.plan.setting_value[0] == 5750 && !result.plan.restart);

    for (unsigned test = 0; test < 20; ++test) {
        snapshot(read); file(read, f); f.settings[2] = 5750;
        switch (test) {
        case 0: f.serial[0] ^= 1; break;
        case 1: f.unit_id[0] ^= 1; break;
        case 2: f.firmware++; break;
        case 3: f.schema_firmware++; break;
        case 4: f.version[5] ^= 1; break;
        case 5: f.version_bytes = 10; break;
        case 6: f.model++; break;
        case 7: f.unit_info[2]++; break;
        case 8: f.unit_const[0] = 0xFF; break;
        case 9: f.records[2][6] ^= 1; break;
        case 10: f.access[2] ^= 1; break;
        case 11: f.block[0] ^= 1; break;
        case 12: f.grid_present = true; break;
        case 13: f.grid_len = 1; break;
        case 14: f.assistant_bytes -= 64; break;
        case 15: f.schema_version = 4; f.unit_version = 3; break;
        case 16: read.failed = 1; break;
        case 17: read.s[3].state = Requester::NOT_READ; break;
        case 18: read.grid_availability = Requester::GridAvailability::UNKNOWN; break;
        case 19: read.assistants.ended = false; break;
        }
        CHECK(!rvsc::check(f, read, result, utc) && !result.ok);
    }

    // Strict genuine eight-byte profile: format acceptance of a synthetic tail cannot qualify writes.
    snapshot(read, 10); file(read, f); f.settings[2] = 5750;
    CHECK(!rvsc::check(f, read, result, utc) && !result.ok);

    for (unsigned test = 0; test < 7; ++test) {
        snapshot(read); file(read, f); f.settings[2] = 5750;
        switch (test) {
        case 0: read.firmware++; break;
        case 1: read.unit_info[0]++; break;
        case 2: read.version[5] ^= 1; break;
        case 3: read.version_bytes = 0; break;
        case 4: read.version_bytes = 7; break;
        case 5: read.version_bytes = 9; break;
        case 6: read.version_bytes = 11; read.version_ok = true; break;
        }
        CHECK(!rvsc::check(f, read, result, utc) && !result.ok);
    }

    snapshot(read); file(read, f); f.settings[2] = 4799;
    CHECK(!rvsc::check(f, read, result, utc));
    f.settings[2] = 6401; CHECK(!rvsc::check(f, read, result, utc));
    file(read, f); f.settings[4] = 36; CHECK(!rvsc::check(f, read, result, utc));
    f.settings[4] = 34; CHECK(rvsc::check(f, read, result, utc));
    file(read, f); f.settings[0] = 8; CHECK(!rvsc::check(f, read, result, utc));
    file(read, f); f.settings[21] = 1; CHECK(!rvsc::check(f, read, result, utc));
    file(read, f); f.settings[20] = uint16_t(-501); CHECK(!rvsc::check(f, read, result, utc));
    f.settings[20] = uint16_t(-500); CHECK(rvsc::check(f, read, result, utc));
    f.settings[20] = 500; CHECK(rvsc::check(f, read, result, utc));
    f.settings[20] = 501; CHECK(!rvsc::check(f, read, result, utc));

    // Protected and restart-required numeric changes retain the ordinary safeguards.
    file(read, f); f.settings[81] = 2;
    CHECK(!rvsc::check(f, read, result, utc));
    f.credentials.count = 1; memset(f.credentials.bytes[0], 0xA5, sizeof f.credentials.bytes[0]);
    f.credentials.valid_time = f.credentials.settings_time = utc;
    CHECK(rvsc::check(f, read, result, utc) && result.plan.required_access == 1 &&
          result.plan.access.count == 1 && result.plan.settings == 1);
    f.credentials.settings_time = utc - 86401;
    CHECK(!rvsc::check(f, read, result, utc));
    f.credentials.settings_time = utc + 1;
    CHECK(!rvsc::check(f, read, result, utc));
    f.credentials.settings_time = utc; f.credentials.valid_time = utc + 86401;
    CHECK(!rvsc::check(f, read, result, utc));
    f.credentials.valid_time = utc;
    CHECK(!rvsc::check(f, read, result, 0));
    file(read, f); f.settings[10] = 2;
    CHECK(rvsc::check(f, read, result, utc) && result.plan.restart && !result.plan.required_access);
    file(read, f); f.settings[0] = 2;
    CHECK(rvsc::check(f, read, result, utc) && result.plan.restart && result.plan.settings == 1);
    read.block[0][2] = 2; file(read, f); f.settings[0] = 4;
    CHECK(!rvsc::check(f, read, result, utc));
    f.credentials.count = 1; f.credentials.valid_time = f.credentials.settings_time = utc;
    CHECK(rvsc::check(f, read, result, utc) && result.plan.required_access == 2);

    // Replacement requires a coherent presence flag and the complete live capacity.
    file(read, f); f.assistants[0] = 5;
    CHECK(rvsc::check(f, read, result, utc) && !result.plan.assistants &&
          result.plan.retain_assistants && !result.plan.disable_assistants && result.plan.image[0] == 0xFF);
    f.unit_info[8] = 0xE0;
    f.credentials.count = 1; f.credentials.valid_time = f.credentials.settings_time = utc;
    f.data[0] = 1;
    CHECK(rvsc::check(f, read, result, utc) && result.plan.assistants && result.plan.restart &&
          result.plan.records == 64 && result.adds_assistants && result.plan.unit_marked[7] == 0xF0 &&
          result.plan.unit_final[7] == 0xE0 && result.plan.image[0] == 5 && !result.plan.blank[0]);
    f.assistants[4095] = 0xA5; f.data[63] = 1;
    CHECK(rvsc::check(f, read, result, utc) && result.plan.image[4095] == 0xA5 && !result.plan.blank[63]);
    f.assistant_bytes -= 64;
    CHECK(!rvsc::check(f, read, result, utc));
    file(read, f); f.assistants[0] = 5; f.unit_info[8] = 0xE0; f.data[0] = 1;
    read.unit_info[8] = 0xD0;
    CHECK(!rvsc::check(f, read, result, utc));
    read.unit_info[8] = 0xF0;
    // A short bank cannot inherit qualification even when file and read agree.
    read.assistants.count = 63; file(read, f);
    f.assistants[0] = 5; f.data[0] = 1; f.unit_info[8] = 0xE0;
    CHECK(f.assistant_bytes == 4032 && !rvsc::check(f, read, result, utc));
    file(read, f); f.settings[2] = 5750;
    CHECK(rvsc::check(f, read, result, utc)); // ordinary settings keep their existing policy
    read.assistants.count = 0;
    CHECK(!rvsc::check(f, read, result, utc));
    read.assistants.count = 64;
    file(read, f); f.unit_info[8] = 0xE0;
    CHECK(!rvsc::check(f, read, result, utc));
    read.unit_info[8] = 0xE0; file(read, f); f.unit_info[8] = 0xF0;
    CHECK(rvsc::check(f, read, result, utc) && !result.plan.assistants &&
          result.plan.disable_assistants && result.plan.retain_assistants && result.plan.restart &&
          result.removes_assistants && result.plan.unit_final[7] == 0xF0);
    read.assistants.len[0] = 1; read.assistants.data[0][0] = 5;
    read.assistants.len[63] = 64; memset(read.assistants.data[63], 0xFF, 64); read.assistants.data[63][63] = 0xA5;
    file(read, f); memset(f.assistants, 0xFF, sizeof f.assistants); memset(f.data, 0, sizeof f.data);
    f.unit_info[8] = 0xF0;
    CHECK(rvsc::check(f, read, result, utc) && !result.plan.assistants && result.plan.disable_assistants &&
          result.plan.retain_assistants && result.plan.image[0] == 5 && result.plan.image[4095] == 0xA5);
    // Native Venus ignores both the empty six-byte header and arbitrary inactive
    // file data. Only the original live bank is retained in the executable plan.
    const uint8_t empty_header[] = {0xA7, 0xFE, 0, 0, 0x57, 0x01};
    memcpy(f.assistants, empty_header, sizeof empty_header);
    CHECK(rvsc::check(f, read, result, utc) && result.plan.disable_assistants &&
          result.plan.image[0] == 5 && result.plan.image[4095] == 0xA5);
    memset(f.assistants, 0x3C, sizeof f.assistants);
    CHECK(rvsc::check(f, read, result, utc) && result.plan.disable_assistants && result.plan.image[0] == 5);
    read.unit_info[8] = 0xF0;
    CHECK(rvsc::check(f, read, result, utc) && !result.plan.disable_assistants && !result.plan.assistants &&
          result.plan.retain_assistants && !result.plan.restart && !result.plan.settings &&
          result.plan.image[4095] == 0xA5);
    f.settings[2] = 5750;
    CHECK(rvsc::check(f, read, result, utc) && result.plan.settings == 1 &&
          result.plan.retain_assistants && !result.plan.disable_assistants && result.plan.image[0] == 5);
    // Reused Check objects must not retain a disable operation on another file.
    file(read, f); f.unit_info[8] = 0xE0; f.assistants[0] ^= 1; f.data[0] = 1;
    CHECK(rvsc::check(f, read, result, utc) && result.plan.assistants &&
          !result.plan.retain_assistants && !result.plan.disable_assistants);

    printf(failures ? "%d FAILED\n" : "all synthetic legacy settings-write guards passed\n", failures);
    return failures ? 1 : 0;
}
