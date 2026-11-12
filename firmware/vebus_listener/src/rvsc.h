// The VE.Bus settings file (.rvsc) Venus's mk2vsc writes for its GUI backup and for VRM's Remote
// VEConfigure download, built from the listener's own read (requester.h, read_settings()), and read
// back: a file to write to the Multi is parsed and checked against a fresh read (upload.h). Plain C++,
// so test/rvsc_test.cpp can run it on a PC: from the Multi's replies recorded during Venus's backup
// (evidence venus-backup) it rebuilds that backup byte for byte, given its save time.
//
// The format, and the reply each byte comes from: tools/vebus_timing/rvsc.py, which rebuilds the same
// file from the bus log, and docs/vebus_config_upload_2026-10-26.md. One unit; the container is
// kylehart/mk2vsc's docs/FORMAT.md (MIT).
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "requester.h"

namespace rvsc {

using vebus::Requester;
constexpr size_t MAX_SIZE = 12288;  // 6560 bytes on this Multi

// Whether the read holds everything the file needs; if not, *why says what is missing.
// This checks backup completeness, independently of model/firmware write qualification.
bool ready(const Requester::SettingsRead &r, const char **why);

// The file, stamped as saved at `unix_time`: its size, or 0 if the read isn't ready or `cap` is short.
size_t build(const Requester::SettingsRead &r, uint32_t unix_time, uint8_t *out, size_t cap);

// A file to write to the Multi, including VEConfigure's optional access credentials and timestamps.
struct File {
    char mk2vsc[8] = "";
    uint8_t schema_version = 0, unit_version = 0;
    uint8_t version_bytes = 0;     // actual stored length: legacy unit 2 accepts eight or ten
    bool grid_present = false;     // an omitted legacy field is different from a present empty blob
    uint32_t schema_firmware = 0;  // the schema header's firmware word
    uint8_t model = 0;             // and its model byte
    uint8_t records[Requester::SETTINGS][10] = {};
    uint8_t block[Requester::BLOCK_ARGS * Requester::BLOCK_BYTES] = {};
    uint8_t access[Requester::SETTINGS] = {};
    int grid_len = 0;
    uint8_t grid[Requester::MAX_GRID_RECORDS * Requester::RECORD] = {};
    uint8_t unit_id[4] = {}, unit_const[3] = {};
    uint32_t firmware = 0;
    uint8_t version[10] = {}, unit_info[10] = {};
    char serial[16] = "";
    bool upload_form = false;
    uint32_t saved = 0;
    vebus::SettingsAccess credentials;
    uint16_t settings[Requester::SETTINGS] = {};
    int assistant_bytes = 0;  // the assistant memory its runs describe (2816 on this Multi)
    uint8_t assistants[Requester::MAX_RECORDS * Requester::RECORD] = {};  // blank bytes 0xFF
    uint8_t data[Requester::MAX_RECORDS] = {};  // bytes of each record given as data (0: blank)
};

// Parses a bounded single-unit legacy 2/2 or modern 4/3 file. Legacy writes require
// the separately qualified eight-byte live identity, including Assistant replacement.
// A legacy F0 target disables Assistants while preserving the complete stored bank;
// that inactive file payload is never programmed, and the original bank is verified.
bool parse(const uint8_t *f, size_t n, File &out, char *why, size_t cap);

// What writing a parsed file to the Multi takes, from a complete read of it made just before. The
// file must be for this unit (serial number, ID, firmware, unit bytes), with the Multi's own schema,
// 0x3C block and grid info, the assistant memory's size, no value for a setting the Multi lacks,
// valid embedded authorization for protected changes, and changed values within their ranges/masks.
struct Check {
    bool ok = false;         // it may be written (perhaps with nothing to write)
    char why[120] = "";      // if not
    Requester::WritePlan plan;
    int assistant_bytes_differ = 0, assistant_first_differ = -1;
    bool removes_assistants = false, adds_assistants = false;  // the unit's assistant flag changes
};
bool check(const File &f, const Requester::SettingsRead &r, Check &out, int64_t utc = 0);

}  // namespace rvsc
