#include "rvsc.h"

#include <stdio.h>
#include <string.h>

namespace rvsc {

namespace {

using Read = vebus::Requester::SettingsRead;
using vebus::Requester;

const char MAGIC[] = "VEConfig setting section file";
const char MK2VSC[] = "1.32";                     // the mk2vsc version on the Pi's Venus, as its files carry
const uint8_t UNIT_CONST[] = {0xFF, 0x01, 0x01};  // the same on all 15 units seen, on 9 systems

// A deliberate export-format policy, not evidence that the inverter has no grid
// data. Keep this independent check beside the codec as well as in the requester.
bool legacy_export(const Read &r) {
    const uint8_t observed[] = {0x6B, 0x1B, 0x28, 0x00, 0x08, 0x0D, 0x00, 0x00};
    return r.grid_availability == Requester::GridAvailability::LEGACY_OMITTED &&
           r.firmware == 2628459 && r.unit_ok && r.unit_info[0] == 47 &&
           (r.version_bytes == 8 || r.version_bytes == 10) &&
           memcmp(r.version, observed, sizeof observed) == 0 &&
           !r.grid.ended && r.grid.count == 0;
}

struct Writer {
    uint8_t *p;
    size_t cap, n = 0;
    bool full = false;
    Writer(uint8_t *out, size_t size) : p(out), cap(size) {}
    void bytes(const void *b, size_t k) {
        if (n + k > cap) {
            full = true;
            return;
        }
        memcpy(p + n, b, k);
        n += k;
    }
    void u8(uint8_t v) { bytes(&v, 1); }
    void u16(unsigned v) {
        const uint8_t b[] = {(uint8_t)v, (uint8_t)(v >> 8)};
        bytes(b, sizeof b);
    }
    void u32(uint32_t v) {
        const uint8_t b[] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
        bytes(b, sizeof b);
    }
    void zeros(size_t k) {
        for (size_t i = 0; i < k; i++) u8(0);
    }
    void put16(size_t at, unsigned v) {  // fill in a length written as 0 earlier
        if (at + 2 > n) return;
        p[at] = (uint8_t)v;
        p[at + 1] = (uint8_t)(v >> 8);
    }
    void put32(size_t at, uint32_t v) {
        if (at + 4 > n) return;
        for (int i = 0; i < 4; i++) p[at + i] = (uint8_t)(v >> (8 * i));
    }
};

// A section: u16 name length, name, u32 offset of the next section, payload, u32 checksum, the sum
// of the section's 32-bit little-endian words from its name length on, zero-padded.
size_t begin(Writer &w, const char *name) {
    w.u16((unsigned)strlen(name));
    w.bytes(name, strlen(name));
    size_t next_at = w.n;
    w.u32(0);
    return next_at;
}

void end(Writer &w, size_t start, size_t next_at) {
    if (w.full) return;
    w.put32(next_at, (uint32_t)(w.n + 4));
    uint32_t sum = 0;
    for (size_t i = start; i < w.n; i += 4) {
        uint32_t word = 0;
        for (size_t j = 0; j < 4 && i + j < w.n; j++) word |= (uint32_t)w.p[i + j] << (8 * j);
        sum += word;
    }
    w.u32(sum);
}

// Winmon 0x05's firmware number, with its eighth byte as the top byte (00 on every file seen; one
// GUI file in kylehart/mk2vsc has c7 in both places).
uint32_t firmware_word(const Read &r) {
    return r.version[0] | (uint32_t)r.version[1] << 8 | (uint32_t)r.version[2] << 16 | (uint32_t)r.version[7] << 24;
}

// The assistant records as runs: u16 n and n bytes, then ff and u16 blank bytes, alternating to the
// end of the records; a record's blank bytes are those after its data.
void assistant_area(Writer &w, const Requester::Records<Requester::MAX_RECORDS> &x) {
    size_t lit_at = w.n;
    unsigned lit = 0, fill = 0;
    w.u16(0);
    for (int i = 0; i < x.count; i++) {
        unsigned len = x.len[i];
        if (len && fill) {
            w.put16(lit_at, lit);
            w.u8(0xFF);
            w.u16(fill);
            lit_at = w.n;
            lit = fill = 0;
            w.u16(0);
        }
        w.bytes(x.data[i], len);
        lit += len;
        fill += Requester::RECORD - len;
    }
    w.put16(lit_at, lit);
    w.u8(0xFF);
    w.u16(fill);
}

}  // namespace

bool ready(const Read &r, const char **why) {
    const char *w = nullptr;
    const bool legacy = legacy_export(r);
    if (r.inspection) {
        w = "the setup read does not contain a full backup";
    } else if (!r.done) {
        w = r.active ? "the read is still running" : "the Multi has not been read yet";
    } else if (r.problem) {
        w = r.problem;
    } else if (!r.serial[0]) {
        w = "no serial number";
    } else if (!r.version_ok && !legacy) {
        w = "no firmware number";
    } else if (!legacy && r.version_bytes != 0 && r.version_bytes != 10) {
        w = "the firmware version record has an unsupported length";
    } else if (!r.unit_ok) {
        w = "no answer to the unit query (21 .. 05)";
    } else if (r.failed) {
        w = "some settings could not be read";
    } else if (r.grid_availability == Requester::GridAvailability::LEGACY_OMITTED && !legacy) {
        w = "the legacy export format is not verified for this read";
    } else if (!r.grid.ended && !legacy) {
        w = "the grid info records are incomplete";
    } else if (!r.assistants.ended) {
        w = "the assistant records are incomplete";
    }
    for (int i = 0; !w && i < Requester::BLOCK_ARGS; i++) {
        if (r.block_got[i] != 3) w = "Winmon 0x3C is incomplete";
    }
    for (int i = 0; !w && i < Requester::SETTINGS; i++) {
        if (r.s[i].state != Requester::READ_OK && r.s[i].state != Requester::UNSUPPORTED) w = "a setting was not read";
    }
    if (!w && (r.grid.count < 0 || r.grid.count > Requester::MAX_GRID_RECORDS ||
               r.assistants.count <= 0 || r.assistants.count > Requester::MAX_RECORDS)) {
        w = "the memory record count is outside the backup bounds";
    }
    for (int i = 0; !w && i < r.grid.count; i++)
        if (r.grid.len[i] > Requester::RECORD) w = "a grid record exceeds the backup bounds";
    for (int i = 0; !w && i < r.assistants.count; i++)
        if (r.assistants.len[i] > Requester::RECORD) w = "an Assistant record exceeds the backup bounds";
    if (why) *why = w ? w : "";
    return !w;
}

size_t build(const Read &r, uint32_t unix_time, uint8_t *out, size_t cap) {
    if (!ready(r, nullptr)) return 0;
    const bool legacy = legacy_export(r);
    Writer w(out, cap);
    w.u16(sizeof MAGIC - 1);
    w.bytes(MAGIC, sizeof MAGIC - 1);

    size_t start, next_at;
    if (!legacy) {
        start = w.n;
        next_at = begin(w, "Mk2vscInfo");
        w.u32(1);
        w.u16(sizeof MK2VSC - 1);
        w.bytes(MK2VSC, sizeof MK2VSC - 1);
        end(w, start, next_at);
    }

    // The schema: GetSettingInfo for settings 0-191 (zeros where not supported), Winmon 0x3C, each
    // setting's access byte, the grid info records.
    const uint32_t fw = firmware_word(r);
    start = w.n;
    next_at = begin(w, "BareSettingInfo");
    w.u32(legacy ? 2 : 4);
    w.u32(fw);
    w.u8(r.unit_info[0]);
    w.u16(Requester::SETTINGS * 10);
    for (const Requester::Setting &s : r.s) {
        bool ok = s.state != Requester::UNSUPPORTED;
        w.u16(ok ? (uint16_t)s.scale : 0);
        w.u16(ok ? (uint16_t)s.offset : 0);
        w.u16(ok ? s.def : 0);
        w.u16(ok ? s.min : 0);
        w.u16(ok ? s.max : 0);
    }
    w.u16(Requester::BLOCK_ARGS * Requester::BLOCK_BYTES);
    w.bytes(r.block, sizeof r.block);
    w.u16(Requester::SETTINGS);
    for (const Requester::Setting &s : r.s) w.u8(s.state != Requester::UNSUPPORTED ? s.access : 0);
    if (!legacy) {
        unsigned grid = 0;
        for (int i = 0; i < r.grid.count; i++) grid += r.grid.len[i];
        w.u16(grid);
        for (int i = 0; i < r.grid.count; i++) w.bytes(r.grid.data[i], r.grid.len[i]);
    }
    end(w, start, next_at);

    // The unit: its ID, firmware and model bytes, serial number, save time, settings, assistants.
    start = w.n;
    next_at = begin(w, "BareSettingData");
    w.u32(legacy ? 2 : 3);
    w.bytes(r.unit_id, sizeof r.unit_id);
    w.u32(fw);
    if (legacy) w.u8(1);
    else w.bytes(UNIT_CONST, sizeof UNIT_CONST);
    w.bytes(r.version, 4);
    const unsigned version_tail = legacy ? r.version_bytes - 4 : 6;
    w.u16(version_tail);
    w.bytes(r.version + 4, version_tail);
    w.bytes(r.unit_info, sizeof r.unit_info);
    size_t serial = strlen(r.serial);
    w.u16((unsigned)serial);
    w.bytes(r.serial, serial);
    w.zeros(10);
    w.u32(unix_time);
    w.zeros(4);
    w.u16(Requester::SETTINGS * 2);
    for (const Requester::Setting &s : r.s) w.u16(s.state == Requester::READ_OK ? s.value : 0);
    assistant_area(w, r.assistants);
    end(w, start, next_at);
    return w.full ? 0 : w.n;
}

// ---------------------------------------------------------------- reading a file back

namespace {

// Reads a payload front to back; any read past its end marks it bad.
struct Reader {
    const uint8_t *p;
    size_t n, i = 0;
    bool bad = false;
    Reader(const uint8_t *data, size_t size) : p(data), n(size) {}
    bool has(size_t k) {
        if (i + k > n) bad = true;
        return !bad;
    }
    void bytes(void *out, size_t k) {
        if (has(k)) memcpy(out, p + i, k);
        i += k;
    }
    unsigned u8() { return has(1) ? p[i++] : (i++, 0); }
    unsigned u16() {
        unsigned v = has(2) ? (unsigned)(p[i] | p[i + 1] << 8) : 0;
        i += 2;
        return v;
    }
    uint32_t u32() {
        uint32_t v = has(4) ? (uint32_t)(p[i] | p[i + 1] << 8 | p[i + 2] << 16 | (uint32_t)p[i + 3] << 24) : 0;
        i += 4;
        return v;
    }
    int64_t time64() {
        uint64_t lo = u32(), hi = u32();
        return (int64_t)(lo | (hi << 32));
    }
    bool at_end() const { return !bad && i == n; }
};

uint32_t u32_at(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

uint32_t word_sum(const uint8_t *b, size_t n) {
    uint32_t sum = 0;
    for (size_t i = 0; i < n; i += 4) {
        uint32_t word = 0;
        for (size_t j = 0; j < 4 && i + j < n; j++) word |= (uint32_t)b[i + j] << (8 * j);
        sum += word;
    }
    return sum;
}

bool fail(char *why, size_t cap, const char *text) {
    snprintf(why, cap, "%s", text);
    return false;
}

bool parse_schema(Reader &r, File &f) {
    const uint32_t version = r.u32();
    if (version != 2 && version != 4) return false;
    f.schema_version = (uint8_t)version;
    f.schema_firmware = r.u32();
    f.model = (uint8_t)r.u8();
    if (r.u16() != sizeof f.records) return false;
    r.bytes(f.records, sizeof f.records);
    if (r.u16() != sizeof f.block) return false;
    r.bytes(f.block, sizeof f.block);
    if (r.u16() != sizeof f.access) return false;
    r.bytes(f.access, sizeof f.access);
    f.grid_present = version != 2;
    if (f.grid_present) {
        f.grid_len = (int)r.u16();
        if (f.grid_len > (int)sizeof f.grid) return false;
        r.bytes(f.grid, (size_t)f.grid_len);
    }
    return r.at_end();
}

// The unit: the header, serial number, the form's bytes, the settings, then the assistant area's runs.
const char *parse_unit(Reader &r, File &f) {
    const uint32_t version = r.u32();
    if (version != (f.schema_version == 2 ? 2u : 3u)) return "the schema and unit versions do not match";
    f.unit_version = (uint8_t)version;
    r.bytes(f.unit_id, 4);
    f.firmware = r.u32();
    if (version == 2) {
        f.unit_const[0] = (uint8_t)r.u8();
        if (f.unit_const[0] != 1) return "the legacy unit marker is not 1";
    } else {
        r.bytes(f.unit_const, 3);
    }
    r.bytes(f.version, 4);
    const unsigned extra = r.u16();
    if (extra != 6 && !(version == 2 && extra == 4)) return "the unit block's firmware bytes are not as expected";
    f.version_bytes = (uint8_t)(4 + extra);
    r.bytes(f.version + 4, extra);
    r.bytes(f.unit_info, 10);
    unsigned sl = r.u16();
    if (sl == 0 || sl >= sizeof f.serial) return "no serial number";
    r.bytes(f.serial, sl);
    f.serial[sl] = 0;
    f.credentials.count = r.u16();
    if (r.bad || f.credentials.count > vebus::SettingsAccess::MAX) return "invalid embedded credential count";
    for (unsigned i = 0; i < f.credentials.count; i++) {
        if (r.u16() != vebus::SettingsAccess::BYTES) return "embedded access credentials must contain eight bytes";
        r.bytes(f.credentials.bytes[i], vebus::SettingsAccess::BYTES);
        if (r.bad) return "the file ends inside an embedded access credential";
    }
    f.credentials.valid_time = r.time64();
    f.credentials.settings_time = r.time64();
    if (r.bad) return "the file ends inside its authorization timestamps";
    f.upload_form = f.credentials.count != 0;
    f.saved = (uint32_t)f.credentials.settings_time;
    if (r.u16() != sizeof f.settings) return "the settings are not 192";
    for (uint16_t &v : f.settings) v = (uint16_t)r.u16();
    if (r.bad) return "the file ends inside the unit block";
    // u16 n and n bytes of data, then ff and a u16 count of blank bytes, alternating to the end. A
    // last fill of none may be marked 00 instead (VEConfigure 3 ends with 00 00 00).
    size_t at = 0, cap = sizeof f.assistants;
    while (!r.bad && r.i < r.n) {
        unsigned lit = r.u16();
        if (r.bad || at + lit > cap || !r.has(lit)) return "the assistant area's runs do not fit";
        for (unsigned k = 0; k < lit; k++, at++) {
            f.assistants[at] = r.p[r.i + k];
            f.data[at / Requester::RECORD]++;
        }
        r.i += lit;
        unsigned marker = r.u8(), fill = r.u16();
        if (r.bad || at + fill > cap) return "the assistant area's runs do not fit";
        if (marker != 0xFF && fill) return "the assistant area holds a fill this cannot write";
        memset(f.assistants + at, 0xFF, fill);
        at += fill;
    }
    if (r.bad || !r.at_end()) return "the assistant area's runs do not end with the block";
    if (at == 0 || at % Requester::RECORD) return "the assistant area is not whole records";
    f.assistant_bytes = (int)at;
    return nullptr;
}

}  // namespace

bool parse(const uint8_t *data, size_t n, File &f, char *why, size_t cap) {
    // Every member starts at zero. Not `f = File()`: that 7 kB temporary overflowed the web server's
    // 8 kB stack on the ESP32.
    memset(static_cast<void *>(&f), 0, sizeof f);
    const size_t magic = sizeof MAGIC - 1;
    if (n < 2 + magic || data[0] != magic || data[1] != 0 || memcmp(data + 2, MAGIC, magic) != 0) {
        return fail(why, cap, "not a VE.Bus settings file (.rvsc)");
    }
    size_t pos = 2 + magic;
    int units = 0;
    bool info = false, schema = false;
    while (pos < n) {
        if (pos + 2 > n) return fail(why, cap, "the file ends inside a section");
        size_t len = data[pos] | data[pos + 1] << 8;
        if (pos + 2 + len + 4 > n) return fail(why, cap, "the file ends inside a section");
        const char *name = (const char *)data + pos + 2;
        size_t next = u32_at(data + pos + 2 + len);
        if (next > n || next < pos + 2 + len + 8) return fail(why, cap, "a section's length is wrong");
        if (word_sum(data + pos, next - 4 - pos) != u32_at(data + next - 4)) {
            snprintf(why, cap, "the checksum of section %.*s is wrong: the file is damaged", (int)(len < 20 ? len : 20),
                     name);
            return false;
        }
        Reader r(data + pos + 2 + len + 4, next - 4 - (pos + 2 + len + 4));
        if (len == 10 && memcmp(name, "Mk2vscInfo", 10) == 0 && !info && !schema && !units) {
            info = true;
            unsigned vl = (r.u32(), r.u16());
            if (vl >= sizeof f.mk2vsc || !r.has(vl)) return fail(why, cap, "the Mk2vscInfo section is not as expected");
            r.bytes(f.mk2vsc, vl);
            f.mk2vsc[vl] = 0;
        } else if (len == 15 && memcmp(name, "BareSettingInfo", 15) == 0 && !schema && !units) {
            schema = true;
            if (!parse_schema(r, f)) return fail(why, cap, "the settings schema (BareSettingInfo) is not as expected");
            if (!info && f.schema_version != 2) return fail(why, cap, "the modern file has no Mk2vscInfo section");
        } else if (len == 15 && memcmp(name, "BareSettingData", 15) == 0 && schema) {
            if (++units > 1) return fail(why, cap, "the file holds several units (.rvms): the ESP32 writes one");
            const char *problem = parse_unit(r, f);
            if (problem) return fail(why, cap, problem);
        } else {
            snprintf(why, cap, "unexpected section %.*s", (int)(len < 20 ? len : 20), name);
            return false;
        }
        pos = next;
    }
    if (!units) return fail(why, cap, "the file holds no unit (BareSettingData)");
    if (why && cap) why[0] = 0;
    return true;
}

namespace {

// A changed value must lie within the setting's range: unsigned when min <= max, else read as signed.
bool in_range(const Requester::Setting &s, uint16_t v) {
    if (s.min <= s.max) return v >= s.min && v <= s.max;
    return (int16_t)v >= (int16_t)s.min && (int16_t)v <= (int16_t)s.max;
}

// Winmon 0x3C supplies one access byte for each bit of these flag settings.
int flag_block(unsigned id) {
    const uint8_t ids[] = {0, 1, 60, 61, 82};
    for (unsigned i = 0; i < sizeof ids; i++) if (id == ids[i]) return (int)i;
    return -1;
}

}  // namespace

bool check(const File &f, const Read &r, Check &out, int64_t utc) {
    out.ok = false;
    out.why[0] = 0;
    out.assistant_bytes_differ = 0;
    out.assistant_first_differ = -1;
    out.removes_assistants = out.adds_assistants = false;
    Requester::WritePlan &p = out.plan;
    p.access.clear();
    p.read_started = r.started;
    p.required_access = 0;
    p.restart = false;
    p.settings = 0;
    p.assistants = false;
    p.records = 0;
    if (f.schema_version != 4 || f.unit_version != 3 || !f.grid_present || f.version_bytes != 10 ||
        r.grid_availability == Requester::GridAvailability::LEGACY_OMITTED) {
        snprintf(out.why, sizeof out.why, "legacy-format exports are read-only; upload qualification is unchanged");
        return false;
    }
    const char *why = nullptr;
    const uint8_t mark = (uint8_t)(r.unit_info[8] & 0x0F);
    if (!ready(r, &why)) {
        snprintf(out.why, sizeof out.why, "the Multi's read is not complete: %s", why);
        return false;
    }
    why = nullptr;
    const uint8_t expected_const[] = {0xFF, 0x01, 0x01};
    if (strcmp(f.serial, r.serial) != 0) {
        snprintf(out.why, sizeof out.why, "the file is for serial number %s; this Multi is %s", f.serial, r.serial);
        return false;
    }
    if (memcmp(f.unit_id, r.unit_id, 4) != 0) why = "the file's unit ID is not this Multi's";
    else if (f.firmware != firmware_word(r) || f.schema_firmware != firmware_word(r) || memcmp(f.version, r.version, 10))
        why = "the file is for another firmware version";
    else if (f.model != r.unit_info[0]) why = "the file is for another model";
    else if (memcmp(f.unit_info, r.unit_info, 8) != 0 || f.unit_info[9] != r.unit_info[9] ||
             (f.unit_info[8] & 0x0F) != mark || (f.unit_info[8] >> 4) < 0x0E)
        why = "the file's unit bytes (phase, unit index) are not this Multi's";
    else if (memcmp(f.unit_const, expected_const, 3) != 0) why = "the file's unit header is not as expected";
    else if (memcmp(f.block, r.block, sizeof f.block) != 0) why = "the file's Winmon 0x3C block is not this Multi's";
    else if (f.assistant_bytes != r.assistants.count * Requester::RECORD)
        why = "the file's assistant area is not the size of this Multi's";
    if (why) {
        snprintf(out.why, sizeof out.why, "%s", why);
        return false;
    }
    for (int i = 0; i < Requester::SETTINGS; i++) {
        const Requester::Setting &s = r.s[i];
        bool ok = s.state != Requester::UNSUPPORTED;
        const uint16_t want[] = {ok ? (uint16_t)s.scale : (uint16_t)0, ok ? (uint16_t)s.offset : (uint16_t)0,
                                 ok ? s.def : (uint16_t)0, ok ? s.min : (uint16_t)0, ok ? s.max : (uint16_t)0};
        bool same = f.access[i] == (ok ? s.access : 0);
        for (int k = 0; k < 5; k++) same = same && (f.records[i][2 * k] | f.records[i][2 * k + 1] << 8) == want[k];
        if (!same) {
            snprintf(out.why, sizeof out.why, "the file's schema differs from this Multi's (setting %d)", i);
            return false;
        }
    }
    int grid = 0;
    for (int i = 0; i < r.grid.count; i++) grid += r.grid.len[i];
    bool grid_same = f.grid_len == grid;
    for (int i = 0, at = 0; grid_same && i < r.grid.count; at += r.grid.len[i], i++) {
        grid_same = memcmp(f.grid + at, r.grid.data[i], r.grid.len[i]) == 0;
    }
    if (!grid_same) {
        snprintf(out.why, sizeof out.why, "the file's grid capability descriptors differ from this Multi's");
        return false;
    }
    for (int i = 0; i < Requester::SETTINGS; i++) {
        const Requester::Setting &s = r.s[i];
        uint16_t v = f.settings[i];
        if (s.state == Requester::UNSUPPORTED) {
            if (v != 0) {
                snprintf(out.why, sizeof out.why, "the file sets setting %d, which this Multi does not have", i);
                return false;
            }
            continue;
        }
        if (v == s.value) continue;
        int flag = flag_block(i);
        uint16_t changed = s.value ^ v;
        // Flag maxima describe writable bits, not a numeric upper bound. Preserve all other bits.
        if (flag >= 0 ? (changed & ~s.max) != 0 : !in_range(s, v)) {
            snprintf(out.why, sizeof out.why, "setting %d: %u is outside its range %u..%u", i, v, s.min, s.max);
            return false;
        }
        uint8_t access = s.access;
        if (flag >= 0) {
            for (unsigned bit = 0; bit < 16; bit++)
                if (changed & (1u << bit)) access |= r.block[flag][bit];
        }
        p.required_access |= access & 0x7F;
        p.restart = p.restart || (access & 0x80) != 0;
        p.setting_id[p.settings] = (uint8_t)i;
        p.setting_value[p.settings] = v;
        p.settings++;
    }
    // The assistant memory, blank bytes as ff on both sides.
    const int records = r.assistants.count;
    for (int i = 0; i < records; i++) {
        for (int k = 0; k < Requester::RECORD; k++) {
            uint8_t mine = k < r.assistants.len[i] ? r.assistants.data[i][k] : 0xFF;
            int at = i * Requester::RECORD + k;
            if (f.assistants[at] != mine) {
                if (out.assistant_first_differ < 0) out.assistant_first_differ = at;
                out.assistant_bytes_differ++;
            }
        }
    }
    bool flag_differs = f.unit_info[8] != r.unit_info[8];
    if (out.assistant_bytes_differ == 0 && flag_differs) {
        snprintf(out.why, sizeof out.why, "the file changes the unit's assistant flag but not its assistants");
        return false;
    }
    if (out.assistant_bytes_differ) {
        p.assistants = true;
        p.restart = true;
        p.records = records;
        memcpy(p.unit_id, r.unit_id, 4);
        memcpy(p.unit_marked, r.unit_info + 1, 9);
        p.unit_marked[7] = (uint8_t)(0xF0 | mark);  // the assistant flag: f = none
        memcpy(p.unit_final, f.unit_info + 1, 9);
        memcpy(p.image, f.assistants, (size_t)records * Requester::RECORD);
        for (int i = 0; i < records; i++) p.blank[i] = f.data[i] == 0;
        out.removes_assistants = (f.unit_info[8] >> 4) == 0x0F && (r.unit_info[8] >> 4) == 0x0E;
        out.adds_assistants = (f.unit_info[8] >> 4) == 0x0E && (r.unit_info[8] >> 4) == 0x0F;
    }
    memcpy(p.unit_id, r.unit_id, sizeof p.unit_id);
    if (p.settings || p.assistants) {
        if (p.required_access && !f.credentials.count) {
            snprintf(out.why, sizeof out.why, "protected changes need embedded authorization: reopen and save in VEConfigure");
            return false;
        }
        if (const char *problem = f.credentials.problem(utc)) {
            snprintf(out.why, sizeof out.why, "%s", problem);
            return false;
        }
        p.access = f.credentials;
    }
    out.ok = true;
    return true;
}

}  // namespace rvsc
