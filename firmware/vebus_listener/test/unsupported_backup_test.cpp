// Exercise the production full-backup reader with unqualified firmware and model
// identities. Reuse the existing deterministic bus simulator; no recorded file,
// physical inverter or credentials are needed.
#define main requester_fixture_suite_main
#include "requester_test.cpp"
#undef main

#include "rvsc.h"

static bool backup_read_command(const uint8_t *p) {
    if (p[0] == 0x21) return p[5] == 0x05 || p[5] == 0x08 || p[5] == 0x0E;
    if (p[0] != 0x00) return false;
    switch (p[2]) {
    case 0x05: case 0x30: case 0x31: case 0x35: case 0x38:
    case 0x3B: case 0x3C: case 0x57: case 0x5F: return true;
    case 0x0E: return p[3] == 0;  // charge-state query, never force a charge stage
    default: return false;
    }
}

static void unqualified_backup(uint32_t firmware, uint8_t model, bool short_version = false,
                               bool missing_setting = false) {
    Requester r;
    AssistantFixture fixture;
    fixture.unit_model = model;
    fixture.version_marker = 0x55;  // a full signature outside the write allowlist
    fixture.version_reply_size = short_version ? 9 : 0;
    const int64_t start = 2000000000;
    r.control(true);
    hear_cycle(r, start - 151 * CYCLE);
    hear_cycle(r, start - CYCLE);
    CHECK(r.read_settings(start));
    drop_info_for = missing_setting ? 40 : -1;
    drops_left = missing_setting ? 3 : 0;
    int cycle = 0;
    for (; cycle < 4000 && !r.settings().done; ++cycle) {
        uint8_t p[16] = {};
        if (play_cycle(r, start + cycle * CYCLE, 4, p, firmware, &fixture)) CHECK(backup_read_command(p));
        if (cycle % 50 == 49) drain(r);
    }
    const auto &read = r.settings();
    CHECK(cycle < 4000 && read.done && !read.active && !read.inspection);
    CHECK(read.firmware == firmware && read.unit_ok && read.unit_info[0] == model);
    const int64_t now = start + cycle * CYCLE;
    CHECK(r.firmware_gate(now) && r.ess_firmware_gate(now) && r.settings_write_firmware_gate(now));

    // A full readable snapshot is independent of qualification. Missing data
    // must still prevent the download rather than yield a partial backup.
    const char *why = nullptr;
    const bool complete = !short_version && !missing_setting;
    Requester::SettingsRead snapshot = read;
    snprintf(snapshot.serial, sizeof snapshot.serial, "BZSIM000000");
    const uint8_t synthetic_id[] = {1, 2, 3, 4};
    memcpy(snapshot.unit_id, synthetic_id, sizeof synthetic_id);
    CHECK(rvsc::ready(snapshot, &why) == complete);
    static uint8_t file[rvsc::MAX_SIZE];
    const size_t size = rvsc::build(snapshot, 1767225800, file, sizeof file);
    CHECK((size != 0) == complete);
    if (complete) {
        static rvsc::File parsed;
        char error[160];
        CHECK(rvsc::parse(file, size, parsed, error, sizeof error));
        CHECK(!strcmp(parsed.serial, "BZSIM000000") && !memcmp(parsed.unit_id, synthetic_id, sizeof synthetic_id));
        CHECK(parsed.firmware == firmware && parsed.model == model);
        CHECK(!memcmp(parsed.version, read.version, sizeof read.version));
        CHECK(parsed.settings[191] == read.s[191].value);
        CHECK(parsed.grid_len > 0 && parsed.assistant_bytes == read.assistants.count * Requester::RECORD);
        uint8_t small[32];
        memset(small, 0xA5, sizeof small);
        CHECK(rvsc::build(snapshot, 1767225800, small + 1, sizeof small - 2) == 0);
        CHECK(small[0] == 0xA5 && small[sizeof small - 1] == 0xA5);

        Requester::WritePlan plan;
        plan.settings = 1;
        plan.setting_id[0] = 2;
        plan.setting_value[0] = read.s[2].value;
        plan.read_started = read.started;
        memcpy(plan.unit_id, read.unit_id, sizeof plan.unit_id);
        CHECK(!r.start_write(now, plan, &why));
        CHECK(!r.write_job().active && r.write_job().settings_written == 0);
        inverter::Command command;
        command.action = inverter::Action::MODE;
        command.value = 7;
        vebus::State state;
        CHECK(!r.operate(now, command, state, error, sizeof error));
        r.target(true, 10, 0);
        for (int i = 0; i < 25; ++i) {
            uint8_t p[16] = {};
            if (play_cycle(r, now + i * CYCLE, 4, p, firmware, &fixture)) CHECK(backup_read_command(p));
        }
        CHECK(r.ctl_stats().writes == 0 && r.ctl_stats().flag_writes == 0);
    } else {
        CHECK(why && *why);
    }
    drain(r);
    drop_info_for = -1;
    drops_left = 0;
}

int main() {
    unqualified_backup(2605570, 0x40);  // 12 V family, unqualified revision
    unqualified_backup(2611556, 0x41);  // 24 V family, unqualified revision
    unqualified_backup(2623570, 0x2F);  // 48 V family, unqualified revision
    unqualified_backup(2799570, 0x60);  // family/model absent from the product catalogue
    unqualified_backup(1915510, 0x61);  // legacy family with the complete backup reply layout
    unqualified_backup(2623570, 0x2F, true);  // insufficient version data is not a complete backup
    unqualified_backup(2623570, 0x2F, false, true);  // unanswered setting cannot be silently omitted
    printf(failures ? "%d FAILED\n" : "unsupported backup tests passed\n", failures);
    return failures ? 1 : 0;
}
