// Checks src/requester.cpp on the PC with made-up bus traffic: when a request may go out, the echo
// and reply bookkeeping, ESS mode, and the conditions that switch it off. Built and run with the
// MinGW compiler PlatformIO installs; the command is in the README.
#include <stdio.h>
#include <string.h>
#include <initializer_list>

#include "assistant_reply.h"
#include "requester.h"
#include "vebus_state.h"
#include "version_reply.h"

using vebus::Requester;
using Mode = vebus::Requester::Mode;

static int failures = 0;
#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            printf("FAIL line %d: %s\n", __LINE__, #cond);             \
            failures++;                                                \
        }                                                              \
    } while (0)

static const int64_t CYCLE = 19967;
static uint8_t nr = 0x10;  // the bus's shared frame counter

static size_t sync_frame(uint8_t *out, uint8_t status = 0xA9) {
    const uint8_t p[] = {0x55, status, 0x34, 0x12};
    return vebus::build_frame(0x83, 0x83, 0xFD, nr++, p, sizeof p, out, 64);
}

static size_t reply_frame(uint8_t id, uint8_t code, uint16_t v0, uint16_t v1, uint8_t *out) {
    const uint8_t p[] = {0x00, id, code, (uint8_t)v0, (uint8_t)(v0 >> 8), (uint8_t)v1, (uint8_t)(v1 >> 8)};
    return vebus::build_frame(0x83, 0x83, 0xFE, nr++, p, sizeof p, out, 64);
}

static size_t ack_frame(uint8_t id, uint8_t *out) {
    const uint8_t p[] = {0x00, id, 0x87};
    return vebus::build_frame(0x83, 0x83, 0xFE, nr++, p, sizeof p, out, 64);
}

static size_t e4_frame(uint8_t *out, uint8_t number) {  // the Multi's status frame, with a chosen number
    const uint8_t p[] = {0xE4, 0x12, 0x34, 0x56};
    return vebus::build_frame(0x83, 0x83, 0xFE, number, p, sizeof p, out, 64);
}

static size_t mk3_frame(uint8_t *out) {
    const uint8_t p[] = {0x00, 0xE5, 0x30, 0x0D, 0x00};
    return vebus::build_frame(0x8F, 0xF7, 0xFE, nr++, p, sizeof p, out, 64);
}

static void hear_sync(Requester &r, int64_t t, uint8_t status = 0xA9) {
    uint8_t f[64];
    size_t n = sync_frame(f, status);
    r.heard(f, n, t);
}

// Two syncs a cycle apart, the second at t: the requester needs both to judge the cycle.
static void hear_cycle(Requester &r, int64_t t, uint8_t status = 0xA9) {
    hear_sync(r, t - CYCLE, status);
    hear_sync(r, t, status);
}

static void drain(Requester &r) {
    char text[1100];
    r.take_report(text, sizeof text);
}

// Send whatever is due at `now`, as main.cpp would on a quiet bus; returns the frame length.
static size_t send(Requester &r, int64_t now, uint8_t *f) {
    size_t n = 0;
    int64_t earliest = 0, latest = 0;
    if (!r.due(now, f, 32, &n, &earliest, &latest)) return 0;
    int64_t start = now > earliest ? now : earliest;
    int64_t t_end = start + (int64_t)n * Requester::BYTE_US + Requester::ADDRESS_PAUSE_US;
    r.sent(f, n, start, t_end);
    nr++;  // our frame takes the next number
    return n;
}

// The next frame from the Multi, counting ours (as on a transceiver that can't hear itself).
static bool next_counts(Requester &r, int64_t t) {
    uint8_t g[64];
    bool counted = false;
    size_t k = e4_frame(g, nr++);
    r.heard(g, k, t, &counted);
    return counted;
}

static void hear_version(Requester &r, uint8_t id, int64_t at, uint32_t firmware = 2623563) {
    uint8_t reply[16], frame[64];
    const size_t size = version_reply(reply, id, firmware);
    const size_t n = vebus::build_frame(0x83, 0x83, 0xFE, nr++, reply, size, frame, sizeof frame);
    r.heard(frame, n, at);
}

static void verify_firmware(Requester &r, int64_t sync) {
    uint8_t frame[32] = {};
    size_t size = send(r, sync + 700, frame);
    if (size < 9 || frame[6] != 0x05)
        printf("Version probe at %lld: frame %zu, command %02x\n", (long long)sync, size, frame[6]);
    CHECK(size >= 9 && frame[6] == 0x05);
    CHECK(next_counts(r, sync + 15030));
    hear_version(r, frame[5], sync + 16080);
    CHECK(!r.firmware_gate(sync + 17000));
}

// The settings the made-up Multi does not have, and a GetSettingInfo it leaves unanswered a few times.
static bool unsupported_setting(int id) { return id == 13 || id == 14 || (id >= 76 && id <= 80); }
static int drop_info_for = -1, drops_left = 0;
// Its grid info: records 0-2 whole, then the end in record 3. Its assistants: record 0 whole, 1 blank,
// 2 with six bytes, then the end.
static const int GRID_END = 3, ASSISTANTS_END = 3;
// A reply the made-up Multi sends in the next cycle (Winmon 0x3C's second): its payload.
static uint8_t later[16];
static size_t later_len = 0;

struct InspectionSetting {
    int id;
    int16_t scale;
    uint16_t def, minimum, maximum, readback;
};

// Descriptors from the checksum-verified 2629556 owner export (Victron Community topic 26745).
// The current values are synthetic and differ from the defaults.
static constexpr InspectionSetting inspection_settings[] = {
    {2, -100, 5760, 4800, 6400, 5700},
    {3, -100, 5520, 4800, 6400, 5480},
    {4, 1, 35, 0, 35, 31},
    {6, -10, 320, 10, 500, 250},
    {64, 1, 0, 0, 65330, 200},
};

static const InspectionSetting *inspection_setting(int id) {
    for (const auto &setting : inspection_settings)
        if (setting.id == id) return &setting;
    return nullptr;
}

struct AssistantFixture {
    uint16_t ram[256] = {};
    int no_reply_at = -1, short_reply_at = -1;
    bool read[256] = {}, read_after_write[256] = {}, written[256] = {};
    bool standalone = true;
    bool inspection = false;
    int unsupported_inspection_setting = -1, out_of_range_inspection_setting = -1;
    unsigned unit_queries = 0;
    uint8_t version_variant = 0, version_marker = 0, unit_model = 0x2F, unit_role = 0;
    size_t version_reply_size = 0;

    AssistantFixture() {
        for (int address = 128; address < 256; ++address) ram[address] = assistant_ram_word(address, 4);
    }
};

// One cycle starting at s, the Multi's part played: hear its sync, send whatever is due, and answer
// it as the Multi would, with `flags` for reads of the flag word (RAM 132). Returns the payload's
// command byte (0x21 for the info and snapshot-trigger requests), or 0 if nothing went out; the
// payload is copied to `sent`.
static uint8_t play_cycle(Requester &r, int64_t s, uint16_t flags, uint8_t *sent = nullptr,
                          uint32_t firmware = 2623563, AssistantFixture *assistant = nullptr) {
    uint8_t f[32], g[64], p[16] = {};
    hear_sync(r, s);
    size_t n = send(r, s + 700, f);
    if (n == 0) {
        if (later_len) {
            size_t k = vebus::build_frame(0x83, 0x83, 0xFE, nr++, later, later_len, g, sizeof g);
            r.heard(g, k, s + 16080);
            later_len = 0;
        }
        r.tick(s + 19000);
        return 0;
    }
    const size_t payload_size = vebus::payload_of(f, n, p, sizeof p);
    if (sent) memcpy(sent, p, sizeof p);
    next_counts(r, s + 15030);
    size_t m = 0;
    if (p[0] == 0x21 && p[5] == 0x05) {  // the unit query: its ID and ten bytes
        uint8_t q[16];
        const size_t size = assistant_unit_reply(q);
        if (assistant) {
            ++assistant->unit_queries;
            q[5] = assistant->unit_model;
            q[13] |= assistant->unit_role;
            if (!assistant->standalone) q[6] = 2;
        }
        m = vebus::build_frame(0x83, 0x83, 0xFE, nr++, q, size, g, sizeof g);
    } else if (p[0] == 0x00 && p[2] == 0x3C) {  // two replies, 0x9E now and 0x9F in the next cycle
        const uint8_t q[] = {0x00, p[1], 0x9E, p[3], 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00};
        m = vebus::build_frame(0x83, 0x83, 0xFE, nr++, q, sizeof q, g, sizeof g);
        const uint8_t q2[] = {0x00, p[1], 0x9F, p[3], 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, (uint8_t)(p[3] + 1)};
        memcpy(later, q2, sizeof q2);
        later_len = sizeof q2;
    } else if (p[0] == 0x00 && (p[2] == 0x57 || p[2] == 0x5F)) {  // a chunk of a record: 11 bytes
        bool grid = p[2] == 0x57;
        int chunk = p[3], rec = p[4];
        uint8_t q[16] = {0x00, p[1], (uint8_t)(grid ? 0xA3 : 0x96)};
        size_t qn = 3 + 11;
        for (int i = 0; i < 11; i++) q[3 + i] = (uint8_t)(rec * 64 + (chunk - 1) * 11 + i);
        if (rec >= (grid ? GRID_END : ASSISTANTS_END)) {
            q[2] = 0x80;  // past the last record
            q[3] = 0x02;
            qn = 4;
        } else if (!grid && rec == 1) {
            qn = 3;  // blank
        } else if (!grid && rec == 2) {
            qn = 3 + 6;
        }
        m = vebus::build_frame(0x83, 0x83, 0xFE, nr++, q, qn, g, sizeof g);
    } else if (p[0] == 0x21 && p[5] == 0x08) {  // an info request: the info frame answers it
        const uint8_t info[] = {0x20, 0x48, 0x39, 0x46, 0x01, (uint8_t)(p[6] == 0xFF ? 0x0C : 0x08), 0x74,
                                0x14, 0x7E, 0x00, 0x00, 0x00, 0x00, 0x00};
        m = vebus::build_frame(0x83, 0x83, 0xFE, nr++, info, sizeof info, g, sizeof g);
    } else if (p[0] == 0x21) {  // the snapshot trigger: no reply
    } else if (p[2] == 0x30) {
        AssistantFixture baseline;
        baseline.ram[132] = flags;
        AssistantFixture &fixture = assistant ? *assistant : baseline;
        uint8_t q[24] = {0x00, p[1], 0x85};
        size_t count = 3;
        bool reply = true;
        for (size_t i = 3; i < payload_size; ++i) {
            const int address = p[i];
            if (address == fixture.no_reply_at) reply = false;
            if (address == fixture.short_reply_at) break;
            fixture.read[address] = true;
            if (fixture.written[address]) fixture.read_after_write[address] = true;
            const uint16_t word = address >= 128 ? fixture.ram[address] : 100;
            q[count++] = word & 0xff;
            q[count++] = word >> 8;
        }
        if (reply) m = vebus::build_frame(0x83, 0x83, 0xFE, nr++, q, count, g, sizeof g);
    } else if (p[2] == 0x37) {
        if (assistant && p[3] == 0) {
            assistant->written[p[4]] = true;
            assistant->ram[p[4]] = p[5] | p[6] << 8;
        }
        m = ack_frame(p[1], g);
    } else if (p[2] == 0x38) {
        m = reply_frame(p[1], 0x99, 0, 0, g);
    } else if (p[2] == 0x3B) {  // the serial number
        const uint8_t q[] = {0x00, p[1], 0x9D, 'H', 'Q', '2', '2', '5', '1', '4', 'A', 'W', 'Y', 'Y'};
        m = vebus::build_frame(0x83, 0x83, 0xFE, nr++, q, sizeof q, g, sizeof g);
    } else if (p[2] == 0x05) {  // the firmware
        uint8_t q[16];
        size_t size = version_reply(q, p[1], firmware);
        if (assistant) {
            if (firmware == 2623556 || firmware == 2629556) q[11] = 0x40;  // genuine 556 exports
            if (assistant->version_marker) q[11] = assistant->version_marker;
            q[10] = assistant->version_variant;
            if (assistant->version_reply_size) size = assistant->version_reply_size;
        }
        m = vebus::build_frame(0x83, 0x83, 0xFE, nr++, q, size, g, sizeof g);
    } else if (p[2] == 0x35) {  // GetSettingInfo: scale -100, offset 0, default 10 x id, 0..65535, access 0x80
        int id = p[3] | p[4] << 8;
        if (assistant && assistant->inspection) {
            const auto *setting = inspection_setting(id);
            uint8_t q[14] = {0x00, p[1], 0x89};
            size_t size = 3;
            if (setting && id != assistant->unsupported_inspection_setting) {
                const uint16_t words[] = {(uint16_t)setting->scale, 0, setting->def, setting->minimum, setting->maximum};
                for (uint16_t word : words) {
                    q[size++] = (uint8_t)word;
                    q[size++] = (uint8_t)(word >> 8);
                }
                q[size++] = 0;  // access
            }
            m = vebus::build_frame(0x83, 0x83, 0xFE, nr++, q, size, g, sizeof g);
        } else if (id == drop_info_for && drops_left > 0) {
            drops_left--;
        } else if (unsupported_setting(id)) {
            const uint8_t q[] = {0x00, p[1], 0x89};
            m = vebus::build_frame(0x83, 0x83, 0xFE, nr++, q, sizeof q, g, sizeof g);
        } else {
            const uint8_t q[] = {0x00, p[1], 0x89, 0x9C, 0xFF, 0x00, 0x00, (uint8_t)(id * 10), (uint8_t)(id * 10 >> 8),
                                 0x00, 0x00, 0xFF, 0xFF, 0x80};
            m = vebus::build_frame(0x83, 0x83, 0xFE, nr++, q, sizeof q, g, sizeof g);
        }
    } else if (p[2] == 0x31) {  // ReadSetting: 3 x id
        int id = p[3] | p[4] << 8;
        const auto *setting = assistant && assistant->inspection ? inspection_setting(id) : nullptr;
        uint16_t value = setting ? setting->readback : (uint16_t)(3 * id);
        if (setting && id == assistant->out_of_range_inspection_setting) value = setting->maximum + 1;
        m = reply_frame(p[1], 0x86, value, 0, g);
    }
    if (m) r.heard(g, m, s + 16080);
    r.tick(s + 19000);
    return p[0] == 0x21 ? 0x21 : p[2];
}

static int64_t finish_assistant_discovery(Requester &r, int64_t cycle) {
    AssistantFixture baseline;
    for (int i = 0; i < 20 && !r.ess_setpoint_ram(); ++i, cycle += CYCLE) {
        uint8_t payload[16] = {};
        CHECK(play_cycle(r, cycle, 4, payload, 2623563, &baseline) != 0x37);
    }
    CHECK(baseline.unit_queries && baseline.read[128] && baseline.read[130] && baseline.read[135]);
    CHECK(baseline.read[138] && baseline.read[131] && baseline.read[132]);
    CHECK(r.ess_setpoint_ram() == 131);
    return cycle;
}

static unsigned check_assistant_writes(Requester &r, AssistantFixture &fixture, int64_t first_cycle,
                                       int cycles, uint32_t firmware, uint8_t setpoint, uint8_t end) {
    unsigned writes = 0;
    for (int i = 0; i < cycles; ++i) {
        uint8_t p[16] = {};
        const uint8_t command = play_cycle(r, first_cycle + i * CYCLE, 4, p, firmware, &fixture);
        if (command == 0x37) {
            CHECK(setpoint && fixture.unit_queries && fixture.read[end] &&
                  fixture.read[setpoint] && fixture.read[setpoint + 1]);
            CHECK(p[0] == 0 && p[3] == 0 && (p[4] == setpoint || p[4] == setpoint + 1));
            ++writes;
        }
        if (i % 20 == 0) drain(r);
    }
    return writes;
}

static void test_assistant_discovery() {
    constexpr int64_t start = 800000000;
    constexpr int cycles = 320;
    const uint32_t revisions[] = {2623556, 2623563};
    for (uint32_t firmware : revisions) {
        Requester r;
        AssistantFixture fixture;
        // Keep the baseline layout on 563 and shift the ESS record four words on 556.
        uint8_t setpoint = 131, end = 138;
        if (firmware == 2623556) {
            fixture.ram[128] = 0x0005;
            fixture.ram[130] = 0x0000;  // this is data within the preceding record
            fixture.ram[132] = 0;
            fixture.ram[133] = 0;
            fixture.ram[134] = 0x0054;
            fixture.ram[135] = 0;
            fixture.ram[136] = 0;
            setpoint = 135;
            end = 139;
        }
        fixture.ram[setpoint + 1] = 0x2004;
        r.control(true);
        hear_sync(r, start - 4000000);
        hear_cycle(r, start - CYCLE);
        r.target(true, 300, 6);
        CHECK(check_assistant_writes(r, fixture, start, cycles, firmware, setpoint, end) >= 2);
        CHECK(r.ess_setpoint_ram() == setpoint && !r.ess_firmware_gate(start + cycles * CYCLE));
        CHECK((r.firmware_gate(start + cycles * CYCLE) != nullptr) == (firmware == 2623556));
        CHECK(r.ctl_stats().acked > 0 && r.ctl_stats().flag_acks == 1);
        CHECK(fixture.ram[setpoint] == 300 && fixture.ram[setpoint + 1] == 0x2006);
        CHECK(fixture.read_after_write[setpoint + 1] && r.multi_flags() == 0x2006);

        // A reconnect with the same firmware discards the previously verified destinations.
        const int64_t reconnect = start + cycles * CYCLE;
        const uint8_t reset[] = {0x00, 0xFB, 0x30, 0x4F, 0xFF};
        r.heard(reset, sizeof reset, reconnect - 200);
        CHECK(!r.ess_setpoint_ram() && r.ess_firmware_gate(reconnect - 200));
        r.target(true, 300, 6);
        AssistantFixture changed;
        changed.ram[130] = 0;
        CHECK(check_assistant_writes(r, changed, reconnect, cycles, firmware, 0, 0) == 0);
        CHECK(changed.read[130] && !r.ess_setpoint_ram());
    }

    enum class Invalid { Missing, Duplicate, TooSmall, Overflow, Truncated, Silent, Parallel, WrongVariant };
    const Invalid invalid[] = {Invalid::Missing, Invalid::Duplicate, Invalid::TooSmall, Invalid::Overflow,
                               Invalid::Truncated, Invalid::Silent, Invalid::Parallel, Invalid::WrongVariant};
    for (Invalid failure : invalid) {
        Requester r;
        AssistantFixture fixture;
        switch (failure) {
        case Invalid::Missing: fixture.ram[130] = 0; break;
        case Invalid::Duplicate: fixture.ram[135] = 0x0052; break;
        case Invalid::TooSmall: fixture.ram[130] = 0x0051; break;
        case Invalid::Overflow:
            for (int address = 128; address <= 240; address += 16) fixture.ram[address] = 0x00AF;
            break;
        case Invalid::Truncated: fixture.short_reply_at = 132; break;
        case Invalid::Silent: fixture.no_reply_at = 130; break;
        case Invalid::Parallel: fixture.standalone = false; break;
        case Invalid::WrongVariant: fixture.version_variant = 199; break;
        }
        r.control(true);
        hear_sync(r, start - 4000000);
        hear_cycle(r, start - CYCLE);
        r.target(true, 300, 6);
        CHECK(check_assistant_writes(r, fixture, start, cycles, 2623556, 0, 0) == 0);
        CHECK(!r.ess_setpoint_ram() && r.ctl_stats().writes == 0 && r.ctl_stats().flag_writes == 0);
        CHECK(fixture.unit_queries);
        if (failure == Invalid::WrongVariant)
            CHECK(r.ess_discovered_ram(start + cycles * CYCLE) == 131);
    }
}

static void test_48_3000_ess() {
    constexpr uint32_t firmware = 2629556;
    constexpr int64_t start = 810000000;
    constexpr int cycles = 320;
    // Match the two Assistant RAM layouts used in the stock Venus 48/3000 capture.
    const int shifts[] = {0, 7};
    for (int shift : shifts) {
        Requester r;
        AssistantFixture fixture;
        if (shift) {
            memset(fixture.ram + 128, 0, 128 * sizeof fixture.ram[0]);
            fixture.ram[128] = 1 + shift;
            for (int address = 130; address <= 138; ++address)
                fixture.ram[address + shift] = assistant_ram_word(address, 0);
        }
        const uint8_t setpoint = 131 + shift, flag_word = setpoint + 1, end = 138 + shift;
        fixture.ram[flag_word] = 0x3000;
        r.control(true);
        hear_sync(r, start - 4000000);
        hear_cycle(r, start - CYCLE);

        struct Target { int watts, wire_watts, flags; };
        // brain_zero takes wire-direction watts. Venus requests +100 W for import (wire -100 W).
        const Target targets[] = {{-100, -100, 1}, {100, 100, 2}, {0, 0, 4},
                                  {32767, Requester::MAX_CTL_WATTS, 1},
                                  {-32767, -Requester::MAX_CTL_WATTS, 2}, {0, 0, 0}};
        int64_t now = start;
        for (const Target &target : targets) {
            const auto before = r.ctl_stats();
            r.target(true, target.watts, target.flags);
            CHECK(check_assistant_writes(r, fixture, now, cycles, firmware, setpoint, end) >= 2);
            now += cycles * CYCLE;
            CHECK(!r.ess_firmware_gate(now) && r.firmware_gate(now));
            CHECK(r.ess_setpoint_ram() == setpoint && r.ctl_stats().acked > before.acked);
            CHECK(r.ctl_stats().flag_acks == before.flag_acks + 1);
            CHECK((int16_t)fixture.ram[setpoint] == target.wire_watts);
            CHECK(fixture.ram[flag_word] == (0x3000 | target.flags));
            CHECK(fixture.read_after_write[flag_word] && r.multi_flags() == fixture.ram[flag_word]);
        }

        // A new bus session must revalidate the firmware and discover its Assistant again.
        const uint8_t reset[] = {0x00, 0xFB, 0x30, 0x4F, 0xFF};
        r.heard(reset, sizeof reset, now - 200);
        CHECK(r.ess_firmware_gate(now) && !r.ess_setpoint_ram());
        r.target(true, -100, 1);
        AssistantFixture missing;
        missing.ram[130] = 0;
        CHECK(check_assistant_writes(r, missing, now, cycles, firmware, 0, 0) == 0);
        CHECK(missing.read[130] && !r.ess_setpoint_ram());
    }

    // A lookalike version reply can be inspected but cannot authorize writes.
    Requester r;
    AssistantFixture wrong_variant;
    wrong_variant.version_variant = 1;
    r.control(true);
    hear_sync(r, start - 4000000);
    hear_cycle(r, start - CYCLE);
    r.target(true, -100, 1);
    CHECK(check_assistant_writes(r, wrong_variant, start, cycles, firmware, 0, 0) == 0);
    const int64_t now = start + cycles * CYCLE;
    CHECK(r.ess_discovered_ram(now) == 131 && !r.ess_setpoint_ram() && r.ess_firmware_gate(now));
}

static void test_mp2_ess_profile(uint32_t firmware, uint8_t version_marker, uint8_t header) {
    constexpr int64_t start = 815000000;
    constexpr int cycles = 320;
    const uint8_t setpoint = header + 1, flags = header + 2;
    enum class Case { Valid, WrongRevision, WrongVariant, ShortReply, WrongModel, MultiUnit, UnitRole, NoAssistant };
    const Case cases[] = {Case::Valid, Case::WrongRevision, Case::WrongVariant, Case::ShortReply,
                          Case::WrongModel, Case::MultiUnit, Case::UnitRole, Case::NoAssistant};
    for (Case scenario : cases) {
        Requester r;
        AssistantFixture fixture;
        // Reproduce the RAM layout and reply variant from the isolated Venus test.
        memset(fixture.ram + 128, 0, 128 * sizeof fixture.ram[0]);
        if (header > 128) fixture.ram[128] = header - 129;
        if (scenario != Case::NoAssistant) fixture.ram[header] = 0x0052;
        fixture.ram[flags] = 0x3000;
        fixture.version_marker = version_marker;
        if (scenario == Case::WrongVariant) fixture.version_variant = 1;
        if (scenario == Case::ShortReply) fixture.version_reply_size = 12;
        if (scenario == Case::WrongModel) fixture.unit_model = 0x02;
        if (scenario == Case::MultiUnit) fixture.standalone = false;
        if (scenario == Case::UnitRole) fixture.unit_role = 1;

        r.control(true);
        hear_sync(r, start - 4000000);
        hear_cycle(r, start - CYCLE);
        r.target(true, -100, 4);
        const bool valid = scenario == Case::Valid;
        const uint32_t reply = scenario == Case::WrongRevision ? firmware + 1 : firmware;
        const unsigned writes = check_assistant_writes(r, fixture, start, cycles, reply,
                                                        valid ? setpoint : 0, valid ? header + 3 : 0);
        const int64_t now = start + cycles * CYCLE;
        const bool known_signature = scenario != Case::WrongRevision && scenario != Case::WrongVariant &&
                                     scenario != Case::ShortReply;
        CHECK((!r.settings_write_firmware_gate(now)) == known_signature);
        CHECK(r.firmware_gate(now));
        if (valid) {
            CHECK(writes >= 2 && !r.ess_firmware_gate(now) && r.ess_setpoint_ram() == setpoint);
            CHECK(fixture.ram[setpoint] == uint16_t(-100) && fixture.ram[flags] == 0x3004);
            CHECK(r.ctl_stats().acked && r.ctl_stats().flag_acks && fixture.read_after_write[flags]);
        } else {
            CHECK(!writes && !r.ess_setpoint_ram() && !r.ctl_stats().acked && !r.ctl_stats().flag_acks);
        }
    }
}

static void test_read_only_assistant_discovery() {
    constexpr int64_t start = 820000000;
    constexpr int cycles = 320;
    // The synthetic 2685 prefix is outside the product registry and sits at the minimum revision.
    const uint32_t revisions[] = {2615559, 2780556, 2685422};
    for (uint32_t firmware : revisions) {
        Requester r;
        AssistantFixture fixture;
        fixture.unit_model = 0x02;
        fixture.version_reply_size = 9;  // long version reply with only the documented fields
        fixture.ram[128] = 0x0007;
        fixture.ram[136] = 0x0052;
        fixture.ram[137] = 0xFF88;
        fixture.ram[138] = 0x2004;
        r.control(true);
        hear_sync(r, start - 4000000);
        hear_cycle(r, start - CYCLE);
        CHECK(check_assistant_writes(r, fixture, start, cycles, firmware, 0, 0) == 0);
        CHECK(fixture.unit_queries == 1 && fixture.read[128] && fixture.read[136] && fixture.read[139]);
        CHECK(fixture.read[137] && fixture.read[138]);
        const int64_t next = start + cycles * CYCLE;
        CHECK(r.ess_discovered_ram(next) == 137 && !r.ess_setpoint_ram());
        CHECK(r.ess_firmware_gate(next));
        CHECK(!r.ess_discovered_ram(next + 6000000));

        r.target(true, 300, 6);
        CHECK(check_assistant_writes(r, fixture, next, cycles, firmware, 0, 0) == 0);
        CHECK(r.ess_discovered_ram(next + cycles * CYCLE) == 137 && !r.ess_setpoint_ram());
        CHECK(r.ctl_stats().writes == 0 && r.ctl_stats().flag_writes == 0);

        const int64_t reconnect = next + cycles * CYCLE;
        const uint8_t reset[] = {0x00, 0xFB, 0x30, 0x4F, 0xFF};
        r.heard(reset, sizeof reset, reconnect - 200);
        CHECK(!r.ess_discovered_ram(reconnect) && !r.ess_setpoint_ram());
        AssistantFixture missing;
        missing.unit_model = fixture.unit_model;
        missing.ram[128] = 0;
        CHECK(check_assistant_writes(r, missing, reconnect, cycles, firmware, 0, 0) == 0);
        CHECK(missing.unit_queries == 1 && missing.read[128]);
        CHECK(!missing.read[129] && !r.ess_discovered_ram(reconnect + cycles * CYCLE));
    }

    struct Unsupported { uint32_t firmware; size_t reply_size; };
    const Unsupported unsupported[] = {{2615421, 9}, {1915510, 9}, {2815559, 9}, {2615559, 8}};
    for (const auto &version : unsupported) {
        Requester r;
        AssistantFixture fixture;
        fixture.version_reply_size = version.reply_size;
        r.control(true);
        hear_sync(r, start - 4000000);
        hear_cycle(r, start - CYCLE);
        r.target(true, 300, 6);
        CHECK(check_assistant_writes(r, fixture, start, cycles, version.firmware, 0, 0) == 0);
        CHECK(!fixture.unit_queries && !fixture.read[128]);
        CHECK(!r.ess_discovered_ram(start + cycles * CYCLE) && !r.ess_setpoint_ram());
    }

    Requester parallel;
    AssistantFixture fixture;
    fixture.standalone = false;
    parallel.control(true);
    hear_sync(parallel, start - 4000000);
    hear_cycle(parallel, start - CYCLE);
    CHECK(check_assistant_writes(parallel, fixture, start, cycles, 2615559, 0, 0) == 0);
    CHECK(fixture.unit_queries && !fixture.read[128]);
    CHECK(!parallel.ess_discovered_ram(start + cycles * CYCLE));

    // A read-only result cannot become an authorized layout when a later version check
    // returns a full write-compatible reply. The unit must pass the stricter check again.
    Requester promote;
    AssistantFixture generic;
    generic.unit_model = 0x02;
    generic.version_reply_size = 9;
    promote.control(true);
    hear_sync(promote, start - 4000000);
    hear_cycle(promote, start - CYCLE);
    CHECK(check_assistant_writes(promote, generic, start, cycles, 2623563, 0, 0) == 0);
    const int64_t refresh = start + cycles * CYCLE;
    CHECK(promote.ess_discovered_ram(refresh) == 131 && !promote.ess_setpoint_ram());
    generic.version_reply_size = 0;
    promote.target(true, 300, 6);
    constexpr int refresh_cycles = 1600;
    CHECK(check_assistant_writes(promote, generic, refresh, refresh_cycles, 2623563, 0, 0) == 0);
    const int64_t now = refresh + refresh_cycles * CYCLE;
    CHECK(!promote.ess_firmware_gate(now) && generic.unit_queries > 1);
    CHECK(!promote.ess_discovered_ram(now) && !promote.ess_setpoint_ram());
}

static int64_t run_settings_inspection(Requester &r, AssistantFixture &fixture, int64_t start,
                                       uint32_t firmware, int expected_descriptors, int expected_values) {
    r.control(true);
    hear_sync(r, start - 4000000);
    hear_cycle(r, start - CYCLE);
    CHECK(r.read_settings(start, true));

    int descriptors = 0, values = 0, cycles = 0;
    for (; cycles < 150 && r.settings().active; ++cycles) {
        uint8_t p[16] = {};
        const uint8_t command = play_cycle(r, start + cycles * CYCLE, 4, p, firmware, &fixture);
        // No backup-specific commands or configuration writes during commissioning.
        CHECK(command != 0x3C && command != 0x57 && command != 0x5F);
        CHECK(p[0] != 0x38 && p[0] != 0x3F);
        CHECK(p[0] != 0x00 || (p[2] != 0x32 && p[2] != 0x33 && p[2] != 0x34 && p[2] != 0x37));
        if (command == 0x35 || command == 0x31) {
            const int id = p[3] | p[4] << 8;
            CHECK(inspection_setting(id));
            if (command == 0x35) ++descriptors;
            else ++values;
        }
    }
    CHECK(r.settings().done && !r.settings().active && r.settings().inspection);
    CHECK(descriptors == expected_descriptors && values == expected_values);
    return start + cycles * CYCLE;
}

static void test_settings_inspection() {
    constexpr int64_t start = 910000000;
    constexpr uint32_t supported = 2629556;
    {
        Requester r;
        AssistantFixture fixture;
        fixture.inspection = true;
        fixture.version_reply_size = 9;
        const int64_t done = run_settings_inspection(r, fixture, start, supported, 5, 5);
        CHECK(r.inspection_ready(done) && r.ess_firmware_gate(done) && r.firmware_gate(done));
        const auto &settings = r.settings();
        CHECK(settings.firmware == supported && settings.unit_ok && fixture.unit_queries == 1);
        CHECK(settings.read == 5 && settings.unsupported == 0 && settings.failed == 0);
        CHECK(!settings.grid.count && !settings.assistants.count);
        for (const auto &expected : inspection_settings) {
            const auto &actual = settings.s[expected.id];
            CHECK(actual.state == Requester::READ_OK && actual.value == expected.readback);
            CHECK(actual.scale == expected.scale && actual.offset == 0 && actual.def == expected.def &&
                  actual.min == expected.minimum && actual.max == expected.maximum && actual.access == 0);
        }
        CHECK(!r.inspection_ready(done + 6000000));
        hear_sync(r, done + 6000000);
        CHECK(!r.inspection_ready(done + 6000001));
    }
    {
        Requester r;
        AssistantFixture fixture;
        fixture.inspection = true;
        fixture.unsupported_inspection_setting = 64;
        fixture.out_of_range_inspection_setting = 6;
        const int64_t done = run_settings_inspection(r, fixture, start, supported, 5, 4);
        CHECK(r.inspection_ready(done) && r.settings().read == 3 && r.settings().unsupported == 1 &&
              r.settings().failed == 1);
        CHECK(r.settings().s[64].state == Requester::UNSUPPORTED && r.settings().s[6].state == Requester::READ_FAILED);
        uint8_t frame[64];
        const size_t size = mk3_frame(frame);
        r.heard(frame, size, done);
        CHECK(!r.inspection_ready(done + 1000) && !r.read_settings(done + 1000, true));
    }
    struct Rejected { uint32_t firmware; size_t reply_size; bool standalone; };
    const Rejected rejected[] = {
        {2614492, 13, true},  // classic MultiPlus
        {2780556, 13, true},  // MultiPlus-II 2x120 V
        {supported, 8, true},
        {supported, 13, false},
    };
    for (const auto &version : rejected) {
        Requester r;
        AssistantFixture fixture;
        fixture.inspection = true;
        fixture.version_reply_size = version.reply_size;
        fixture.standalone = version.standalone;
        const int64_t done = run_settings_inspection(r, fixture, start, version.firmware, 0, 0);
        CHECK(!r.inspection_ready(done) && r.settings().problem);
    }
}

int main() {
    char text[1100];
    uint8_t f[32], g[64];
    int64_t t = 1000000;
    size_t n = 0;
    int64_t earliest = 0, latest = 0;

    // Off until switched on.
    {
        Requester r;
        hear_cycle(r, t);
        CHECK(!r.due(t + 700, f, sizeof f, &n, &earliest, &latest));
    }

    // A read: after the sync, timed to end at +2.4 ms, numbered one up, answered in the same cycle.
    {
        Requester r;
        r.start(t, 60, Mode::READ);
        CHECK(r.take_report(text, sizeof text) && strstr(text, "# tx on for 60 s"));
        hear_sync(r, t);
        CHECK(!r.due(t + 700, f, sizeof f, &n, &earliest, &latest));  // one sync isn't enough to judge the cycle
        hear_sync(r, t + CYCLE);
        int64_t s = t + CYCLE;
        uint8_t sync_nr = nr - 1;
        CHECK(r.due(s + 700, f, sizeof f, &n, &earliest, &latest));
        CHECK(n == 11 && f[0] == 0x98 && f[1] == 0xF7 && f[2] == 0xFE && f[3] == (uint8_t)(sync_nr + 1));
        CHECK(f[4] == 0x00 && f[5] == 0xC0 && f[6] == 0x30 && f[7] == 0x0D && f[8] == 0x00 && f[10] == 0xFF);
        CHECK(vebus::checksum_ok(f, n));
        CHECK(earliest == s + 2400 - (11 * 40 + 8));
        CHECK(latest == s + 2500 - (11 * 40 + 8));
        CHECK(!r.due(latest, f, sizeof f, &n, &earliest, &latest));  // the window has closed
        CHECK(send(r, s + 700, f) == 11);
        CHECK(r.heard(f, 11, s + 1952));  // the echo
        CHECK(r.stats().echo_ok == 1);
        size_t k = reply_frame(0xC0, 0x85, 88, 23697, g);
        CHECK(!r.heard(g, k, s + 16070));
        CHECK(r.stats().answered == 1 && r.stats().same_cycle == 1 && r.stats().unanswered == 0);
        CHECK(r.stats().last_values[0] == 88 && r.stats().last_values[1] == 23697);
        CHECK(r.take_report(text, sizeof text));
        printf("%s", text);
        CHECK(strstr(text, "read id=C0") && strstr(text, "start=+1.952") && strstr(text, "echo=ok") &&
              strstr(text, "reply=85") && strstr(text, "same cycle") && strstr(text, "SOC 44.0 %, mains 236.97 V"));

        // Not again within the second; a second later, with the next id.
        hear_sync(r, s + CYCLE);
        CHECK(!r.due(s + CYCLE + 700, f, sizeof f, &n, &earliest, &latest));
        int64_t s2 = s + 51 * CYCLE;
        hear_cycle(r, s2);
        CHECK(r.due(s2 + 700, f, sizeof f, &n, &earliest, &latest));
        CHECK(f[5] == 0xC1);
        CHECK(r.stats().gaps == 0 && r.stats().yielded == 0);
    }

    // Another master's frame after the previous cycle's cutoff claims this cycle's reply: yield, once
    // per cycle however often asked, and send in the next cycle.
    {
        Requester r;
        r.start(t, 60, Mode::READ);
        hear_sync(r, t - CYCLE);
        size_t k = mk3_frame(g);
        r.heard(g, k, t - CYCLE + 9200);
        hear_sync(r, t);
        CHECK(!r.due(t + 700, f, sizeof f, &n, &earliest, &latest));
        CHECK(!r.due(t + 900, f, sizeof f, &n, &earliest, &latest));
        CHECK(r.stats().yielded == 1);
        hear_sync(r, t + CYCLE);
        CHECK(r.due(t + CYCLE + 700, f, sizeof f, &n, &earliest, &latest));
        CHECK(f[3] == (uint8_t)nr);
    }

    // One that ended before the previous cutoff belonged to the previous cycle: no claim.
    {
        Requester r;
        r.start(t, 60, Mode::READ);
        hear_sync(r, t - CYCLE);
        size_t k = mk3_frame(g);
        r.heard(g, k, t - CYCLE + 1000);
        hear_sync(r, t);
        CHECK(r.due(t + 700, f, sizeof f, &n, &earliest, &latest));
        CHECK(r.stats().yielded == 0);
    }

    // No echo, but the next frame takes the number after ours: counted, not a gap, and the frame
    // sent is handed out for the decoder.
    {
        Requester r;
        r.start(t, 60, Mode::READ);
        hear_cycle(r, t);
        CHECK(send(r, t + 700, f) == 11);
        CHECK(next_counts(r, t + 15030));
        size_t sn = 0;
        int64_t st = 0;
        const uint8_t *sf = r.sent_frame(&sn, &st);
        CHECK(sn == 11 && memcmp(sf, f, 11) == 0 && st == t + 1952);
        CHECK(r.stats().counted == 1 && r.stats().failed == 0 && r.stats().gaps == 0);
        size_t k = reply_frame(0xC0, 0x85, 78, 23795, g);
        r.heard(g, k, t + 16080);
        CHECK(r.stats().answered == 1);
        CHECK(r.take_report(text, sizeof text) && strstr(text, "echo=counted reply=85"));
    }

    // No echo, and the next frame repeats our number: lost. Three switch it off, after the third's line.
    {
        Requester r;
        r.start(t, 60, Mode::READ);
        drain(r);
        for (int i = 0; i < 3; i++) {
            int64_t ts = t + i * 60 * CYCLE;
            hear_cycle(r, ts);
            CHECK(send(r, ts + 700, f) == 11);
            size_t k = e4_frame(g, f[3]);  // same number as ours
            r.heard(g, k, ts + 15030);
            r.tick(ts + 2000 + 250000);
        }
        CHECK(r.stats().failed == 3 && r.stats().counted == 0 && !r.on());
        CHECK(r.take_report(text, sizeof text) && strstr(text, "echo=lost reply=none\n# tx off (requests failed"));
    }

    // Five unanswered in a row switch it off.
    {
        Requester r;
        r.start(t, 60, Mode::READ);
        for (int i = 0; i < 5; i++) {
            int64_t ts = t + i * 60 * CYCLE;
            hear_cycle(r, ts);
            CHECK(send(r, ts + 700, f) == 11);
            r.heard(f, 11, ts + 1952);
            drain(r);
            r.tick(ts + 2000 + 250000);
        }
        CHECK(r.stats().unanswered == 5 && !r.on());
        CHECK(r.take_report(text, sizeof text) && strstr(text, "tx off (requests went unanswered)"));
    }

    // Bench mode: refused while anyone else is heard, then one a second; an echo or nothing.
    {
        Requester r;
        hear_sync(r, t);
        r.start(t + 100000, 10, Mode::BENCH);
        CHECK(!r.due(t + 200000, f, sizeof f, &n, &earliest, &latest));
        int64_t quiet = t + Requester::BENCH_QUIET_US + 1;
        CHECK(send(r, quiet, f) == 11);
        CHECK(r.heard(f, 11, quiet));
        CHECK(r.stats().echo_ok == 1 && r.stats().answered == 0 && r.stats().unanswered == 0);
        drain(r);
        CHECK(send(r, quiet + 1000000, f) == 11);
        r.tick(quiet + 1000000 + 60000);
        CHECK(r.stats().failed == 1);
        CHECK(r.take_report(text, sizeof text) && strstr(text, "bench") && strstr(text, "echo=lost"));
    }

    // ESS mode: refused beyond +-100 W, and while another master was heard in the last 10 s.
    {
        Requester r;
        r.start(t, 60, Mode::ESS, 150);
        CHECK(!r.on() && r.take_report(text, sizeof text) && strstr(text, "ess refused: +150 W"));
        size_t k = mk3_frame(g);
        r.heard(g, k, t);
        r.start(t + 9000000, 60, Mode::ESS, 10);
        CHECK(!r.on() && r.take_report(text, sizeof text) && strstr(text, "ess refused: another master"));
        r.start(t + 10500000, 60, Mode::ESS, 10);
        CHECK(r.on() && r.mode() == Mode::ESS);
    }

    // ESS mode: the check read, then a write every 2.5 s, acknowledged; then time up.
    {
        Requester r;
        int64_t t0 = 50000000;
        r.start(t0 - CYCLE, 6, Mode::ESS, 10);
        CHECK(r.take_report(text, sizeof text) && strstr(text, "# ess on for 6 s"));
        hear_cycle(r, t0 - CYCLE, 0x8C);  // passthrough
        verify_firmware(r, t0 - CYCLE);
        drain(r);
        const int64_t s = finish_assistant_discovery(r, t0);
        drain(r);
        hear_sync(r, s, 0x8C);
        const size_t written = send(r, s + 700, f);
        const uint8_t want[] = {0x98, 0xF7, 0xFE, f[3], 0x00, f[5], 0x37, 0x00, 0x83, 0x0A, 0x00};
        CHECK(written >= 13 && memcmp(f, want, sizeof want) == 0 && vebus::checksum_ok(f, written));
        CHECK(next_counts(r, s + 15030));
        const size_t k = ack_frame(f[5], g);
        r.heard(g, k, s + 16070);
        CHECK(r.stats().writes == 1 && r.stats().acked == 1);
        CHECK(r.take_report(text, sizeof text) && strstr(text, "write id=") && strstr(text, "reply=87") &&
              strstr(text, "setpoint=+10 W"));
        // Assistant-dependent sync bits must not create claims about the Multi's ESS state.
        for (int i = 1; i <= 4; i++) hear_sync(r, s + i * CYCLE, 0xA9);
        for (int i = 5; i <= 7; i++) hear_sync(r, s + i * CYCLE, 0x98);
        for (int i = 8; i <= 10; i++) hear_sync(r, s + i * CYCLE, 0xA9);
        CHECK(!r.take_report(text, sizeof text));
        // the next write is due 2.5 s after the last
        int64_t s3 = s + 100 * CYCLE;  // 2.0 s later
        hear_cycle(r, s3);
        CHECK(!r.due(s3 + 700, f, sizeof f, &n, &earliest, &latest));
        int64_t s4 = s + 126 * CYCLE;  // 2.5 s later
        hear_cycle(r, s4);
        CHECK(r.due(s4 + 700, f, sizeof f, &n, &earliest, &latest) && f[6] == 0x37);
        CHECK(earliest == s4 + 2400 - (n * 40 + 8));
        // time is up: off, with the passthrough note
        r.tick(t0 + 6000001);
        CHECK(!r.on());
        CHECK(r.take_report(text, sizeof text) && strstr(text, "# ess off (time is up)") && strstr(text, "writes=1 acked=1") &&
              strstr(text, "falls back to passthrough"));
    }

    // ESS mode: a check that finds something else writes nothing; a refused write stops; a negative
    // value is written as a signed 16-bit number.
    {
        Requester r;
        r.start(t - CYCLE, 60, Mode::ESS, 10);
        hear_cycle(r, t - CYCLE);
        verify_firmware(r, t - CYCLE);
        AssistantFixture missing;
        missing.ram[130] = 0x00A2;
        for (int i = 0; i < 25; ++i)
            CHECK(play_cycle(r, t + i * CYCLE, 4, nullptr, 2623563, &missing) != 0x37);
        CHECK(missing.read[130] && missing.read[133] && !r.ess_setpoint_ram() && r.stats().writes == 0);
    }
    {
        Requester r;
        r.start(t - CYCLE, 60, Mode::ESS, -10);
        hear_cycle(r, t - CYCLE);
        verify_firmware(r, t - CYCLE);
        const int64_t s = finish_assistant_discovery(r, t);
        drain(r);
        hear_sync(r, s);
        const size_t written = send(r, s + 700, f);
        CHECK(written > 0);
        uint8_t p[16];
        vebus::payload_of(f, written, p, sizeof p);
        CHECK(p[2] == 0x37 && p[4] == 0x83 && p[5] == 0xF6 && p[6] == 0xFF);
        next_counts(r, s + 15030);
        const uint8_t refusal[] = {0x00, p[1], 0x80};  // not WriteRamOK
        const size_t k = vebus::build_frame(0x83, 0x83, 0xFE, nr++, refusal, sizeof refusal, g, sizeof g);
        r.heard(g, k, s + 16070);
        CHECK(!r.on());
        CHECK(r.take_report(text, sizeof text) && strstr(text, "ess off (the Multi refused the setpoint write)"));
    }

    // ESS mode stops as soon as another master is heard.
    {
        Requester r;
        r.start(t, 60, Mode::ESS, 10);
        hear_cycle(r, t);
        size_t k = mk3_frame(g);
        r.heard(g, k, t + 5000);
        CHECK(!r.on());
        CHECK(r.take_report(text, sizeof text) && strstr(text, "another master is on the VE.Bus"));
    }

    // Time is up for reads: off, with a summary.
    {
        Requester r;
        r.start(t, 2, Mode::READ);
        drain(r);
        r.tick(t + 1999999);
        CHECK(r.on());
        r.tick(t + 2000000);
        CHECK(!r.on());
        CHECK(r.take_report(text, sizeof text) && strstr(text, "tx off (time is up)"));
        char json[512];
        r.to_json(t + 2000000, json, sizeof json);
        printf("%s\n", json);
        CHECK(strstr(json, "\"mode\":\"off\"") && strstr(json, "\"why\":\"time is up\"") && strstr(json, "\"writes\":0"));
    }

    // Requests are capped at 600 s, bench at 60 s.
    {
        Requester r;
        r.start(t, 100000, Mode::READ);
        char line[200];
        r.describe(t, line, sizeof line);
        CHECK(strstr(line, "tx=on(600s)"));
        r.start(t, 0, Mode::READ);
        CHECK(!r.on());
    }

    // The decoder, fed our unheard frame as our own, sees no gap and counts our setpoint write.
    {
        vebus::Decoder d;
        size_t k = sync_frame(g, 0x8C);
        d.feed(g, k, t);
        CHECK(d.state().setpoint_fresh(t) == -1 && d.state().discharge_disabled(t) == -1);
        const uint8_t w[] = {0x00, 0xC1, 0x37, 0x00, 0x83, 0x0A, 0x00};
        k = vebus::build_frame(0x98, 0xF7, 0xFE, nr++, w, sizeof w, f, sizeof f);
        d.feed(f, k, t + 1900, true);
        k = e4_frame(g, nr++);
        d.feed(g, k, t + 15000);
        CHECK(d.state().gaps == 0 && d.state().own_frames == 1 && d.state().master_frames == 0);
        CHECK(d.state().setpoint_w.v == 10 && d.state().setpoint_writes == 1);
    }

    // The controller: off by default; on, it listens for 3 s, then polls the Multi in turn,
    // snapshot first, timed late in the slot.
    {
        Requester r;
        int64_t t0 = 200000000;
        hear_cycle(r, t0);
        CHECK(!r.due(t0 + 700, f, sizeof f, &n, &earliest, &latest));  // off
        r.control(true);
        r.tick(t0 + 800);
        CHECK(r.control_state(t0 + 800) == Requester::CtlState::LISTENING);
        CHECK(!r.due(t0 + 900, f, sizeof f, &n, &earliest, &latest));
        int64_t s = t0 + 151 * CYCLE;  // past the 3 s of listening, no other master heard
        hear_cycle(r, s);
        r.tick(s + 600);
        CHECK(r.control_state(s + 600) == Requester::CtlState::POLLING);
        CHECK(r.take_report(text, sizeof text) && strstr(text, "# control: polling the Multi"));
        verify_firmware(r, s);
        s += CYCLE;
        hear_sync(r, s);
        CHECK(send(r, s + 700, f) == 16);  // 98 F7 FE nr 21 00 00 00 00 0E 0E 10 0F 1A cs FF: trigger a snapshot
        const uint8_t trigger[] = {0x21, 0x00, 0x00, 0x00, 0x00, 0x0E, 0x0E, 0x10, 0x0F, 0x1A};
        CHECK(memcmp(f + 4, trigger, sizeof trigger) == 0);
        CHECK(next_counts(r, s + 15030));  // counted; no reply to wait for
        hear_sync(r, s + CYCLE);
        CHECK(!r.due(s + CYCLE + 700, f, sizeof f, &n, &earliest, &latest) || f[6] != 0x38);  // not read yet
        int64_t s1 = s + 7 * CYCLE;  // read it 120 ms after the trigger
        hear_cycle(r, s1);
        CHECK(send(r, s1 + 700, f) == 9 && f[6] == 0x38);  // 98 F7 FE nr 00 id 38 cs FF
        CHECK(next_counts(r, s1 + 15030));
        size_t k = reply_frame(f[5], 0x99, 0xFFAB, 0x0248, g);  // the snapshot reply
        r.heard(g, k, s1 + 16080);
        CHECK(r.ctl_stats().polls == 3 && r.ctl_stats().answered == 2 && r.ctl_stats().unanswered == 0);
        // next cycle: the DC info request, answered by an info frame with 0x0C in byte 5
        int64_t s2 = s1 + CYCLE;
        hear_sync(r, s2);
        CHECK(send(r, s2 + 700, f) > 0);
        uint8_t p[16];
        vebus::payload_of(f, 16, p, sizeof p);
        CHECK(p[0] == 0x21 && p[5] == 0x08 && p[6] == 0xFF);
        next_counts(r, s2 + 15030);
        const uint8_t dc[] = {0x20, 0x48, 0x39, 0x46, 0x01, 0x0C, 0x74, 0x14, 0x7E, 0x00, 0x00, 0x00, 0x00, 0x00};
        k = vebus::build_frame(0x83, 0x83, 0xFE, nr++, dc, sizeof dc, g, sizeof g);
        r.heard(g, k, s2 + 16080);
        CHECK(r.ctl_stats().answered == 3);
        // then L1 info, then the flag word with the SOC
        hear_sync(r, s2 + CYCLE);
        CHECK(send(r, s2 + CYCLE + 700, f) > 0);
        vebus::payload_of(f, 16, p, sizeof p);
        CHECK(p[0] == 0x21 && p[5] == 0x08 && p[6] == 0x00);
        next_counts(r, s2 + CYCLE + 15030);
        const uint8_t l1[] = {0x20, 0x01, 0x01, 0x01, 0x09, 0x08, 0xB7, 0x5D, 0x69, 0x00, 0xB7, 0x5D, 0x00, 0x00};
        k = vebus::build_frame(0x83, 0x83, 0xFE, nr++, l1, sizeof l1, g, sizeof g);
        r.heard(g, k, s2 + CYCLE + 16080);
        hear_sync(r, s2 + 2 * CYCLE);
        CHECK(send(r, s2 + 2 * CYCLE + 700, f) == 11);
        CHECK(f[6] == 0x30 && f[7] == 0 && f[8] == 13);
        CHECK(r.ctl_stats().answered == 4 && r.ctl_stats().polls == 6);
        r.tick(s2 + 2 * CYCLE + 250000);  // the flags read goes unanswered
        CHECK(r.ctl_stats().unanswered == 1);
        // another master: paused at once, and back 10 s after its last frame
        k = mk3_frame(g);
        r.heard(g, k, s2 + 15 * CYCLE);
        r.tick(s2 + 15 * CYCLE + 100);
        CHECK(r.control_state(s2 + 15 * CYCLE + 100) == Requester::CtlState::PAUSED);
        CHECK(r.take_report(text, sizeof text) && strstr(text, "# control: paused: another master"));
        hear_cycle(r, s2 + 30 * CYCLE);
        CHECK(!r.due(s2 + 30 * CYCLE + 700, f, sizeof f, &n, &earliest, &latest));
        int64_t s3 = s2 + 15 * CYCLE + 10100000;
        hear_cycle(r, s3);
        CHECK(r.due(s3 + 700, f, sizeof f, &n, &earliest, &latest));
    }

    // The controller given a target: the check read first, then a write every 2.5 s, carrying the
    // latest value; stopped, the writes end and polling goes on.
    {
        Requester r;
        int64_t t0 = 300000000;
        r.control(true);
        CHECK(r.last_written() == Requester::NOT_WRITTEN);
        int64_t s = t0 + 501 * CYCLE;
        hear_sync(r, t0);
        hear_cycle(r, s);
        verify_firmware(r, s);
        s += CYCLE;
        hear_sync(r, s);
        r.target(true, 10, -1);
        r.tick(s + 600);
        CHECK(r.control_state(s + 600) == Requester::CtlState::CHECKING);
        s = finish_assistant_discovery(r, s);
        drain(r);
        hear_sync(r, s);
        CHECK(send(r, s + 700, f) >= 13);
        const uint8_t want[] = {0x98, 0xF7, 0xFE, f[3], 0x00, f[5], 0x37, 0x00, 0x83, 0x0A, 0x00};
        CHECK(memcmp(f, want, sizeof want) == 0);
        CHECK(r.last_written() == 10);
        next_counts(r, s + 15030);
        const size_t k = ack_frame(f[5], g);
        r.heard(g, k, s + 16070);
        CHECK(r.ctl_stats().writes == 1 && r.ctl_stats().acked == 1);
        CHECK(r.take_report(text, sizeof text) && strstr(text, "write id=") && strstr(text, "reply=87") &&
              strstr(text, "setpoint=+10 W"));
        // polls in between; the next write 2.5 s after the last, with the value given since
        hear_sync(r, s + CYCLE);
        CHECK(send(r, s + CYCLE + 700, f) > 0 && f[6] != 0x37);
        next_counts(r, s + CYCLE + 15030);
        r.tick(s + CYCLE + 250000);
        r.target(true, -20, -1);
        int64_t s4 = s + 126 * CYCLE;
        hear_cycle(r, s4);
        CHECK(send(r, s4 + 700, f) == 14);  // the 0xFF of -20 goes out escaped, as FA 7F
        vebus::payload_of(f, 14, g, sizeof g);
        CHECK(g[2] == 0x37 && g[4] == 0x83 && g[5] == 0xEC && g[6] == 0xFF);
        CHECK(r.last_written() == -20);
        next_counts(r, s4 + 15030);
        r.tick(s4 + 250000);
        r.target(true, 9000, -1);  // held within the controller's hard limit
        int64_t s5 = s4 + 126 * CYCLE;
        hear_cycle(r, s5);
        CHECK(send(r, s5 + 700, f) == 13);
        vebus::payload_of(f, 13, g, sizeof g);
        CHECK(g[2] == 0x37 && (g[5] | g[6] << 8) == 5000);
        next_counts(r, s5 + 15030);
        r.tick(s5 + 250000);
        r.target(false, -20, -1);
        CHECK(r.take_report(text, sizeof text) && strstr(text, "ESS from here off"));
        CHECK(r.control_state(s5 + 250000) == Requester::CtlState::POLLING);
        int64_t s6 = s5 + 200 * CYCLE;
        hear_cycle(r, s6);
        CHECK(send(r, s6 + 700, f) > 0 && f[6] != 0x37);
        r.tick(s6 + 250000);
        char json[1100];
        r.to_json(s6 + 250000, json, sizeof json);
        CHECK(strstr(json, "\"ctl\":{\"state\":\"polling\",\"armed\":false,\"watts\":-20"));
    }

    // The flag word: read, written with WriteViaID RAM 132 when it differs from the target, then read
    // back; left alone at -1. Another master in between: check and read again before writing.
    {
        Requester r;
        int64_t t0 = 350000000;
        r.control(true);
        hear_sync(r, t0);
        int64_t s = t0 + 501 * CYCLE;
        hear_sync(r, s - CYCLE);
        hear_sync(r, s);
        verify_firmware(r, s);
        s += CYCLE;
        r.target(true, 10, 6);
        CHECK(r.take_report(text, sizeof text) && strstr(text, "the flag word should be 0x06"));
        uint16_t multi = 4;  // the Multi's flag word
        uint8_t p[16];
        int flag_writes = 0, i = 0;
        for (; i < 400 && flag_writes == 0; i++) {
            uint8_t c = play_cycle(r, s + i * CYCLE, multi, p);
            if (c == 0x37 && p[4] == 0x84) {
                flag_writes++;
                CHECK(p[5] == 6 && p[6] == 0);
                multi = 6;
            }
        }
        CHECK(flag_writes == 1 && r.multi_flags() == -1 && r.ctl_stats().flag_acks == 1);
        CHECK(r.take_report(text, sizeof text) && strstr(text, "flags-write id=") && strstr(text, "flags=0x06"));
        // read back soon after, and then nothing more to write
        int reads = 0;
        for (int j = 0; j < 300; j++, i++) {
            uint8_t c = play_cycle(r, s + i * CYCLE, multi, p);
            if (c == 0x30 && p[3] == 132) reads++;
            CHECK(!(c == 0x37 && p[4] == 0x84));
        }
        CHECK(reads >= 1 && r.multi_flags() == 6 && r.ctl_stats().flag_writes == 1);
        CHECK(r.ctl_stats().unanswered == 0 && r.ctl_stats().backoffs == 0);
        // -1: whatever the Multi has stays
        r.target(true, 10, -1);
        multi = 7;
        for (int j = 0; j < 300; j++, i++) {
            uint8_t c = play_cycle(r, s + i * CYCLE, multi, p);
            CHECK(!(c == 0x37 && p[4] == 0x84));
        }
        CHECK(r.multi_flags() == 7);
        // The ESS assistant migrated on 2026-10-27 sets bits 12 and 13: only the low three bits are ours.
        multi = 0x2004;
        for (int j = 0; j < 300; j++, i++) play_cycle(r, s + i * CYCLE, multi, p);  // read, target still -1
        CHECK(r.multi_flags() == 0x2004);
        r.target(true, 10, 4);
        for (int j = 0; j < 300; j++, i++) {
            uint8_t c = play_cycle(r, s + i * CYCLE, multi, p);
            CHECK(!(c == 0x37 && p[4] == 0x84));  // bits 0-2 already 4: nothing to write
        }
        CHECK(r.multi_flags() == 0x2004);
        r.target(true, 10, 6);
        bool kept = false;
        for (int j = 0; j < 400 && !kept; j++, i++) {
            uint8_t c = play_cycle(r, s + i * CYCLE, multi, p);
            if (c == 0x37 && p[4] == 0x84) {
                kept = p[5] == 0x06 && p[6] == 0x20;  // 0x2006: bit 13 as read
                multi = 0x2006;
            }
        }
        CHECK(kept);
        multi = 7;
        // another master: the check and the flag word read come again before any write
        r.target(true, 10, 4);
        size_t k = mk3_frame(g);
        r.heard(g, k, s + i * CYCLE + 3000);
        r.tick(s + i * CYCLE + 4000);
        CHECK(r.control_state(s + i * CYCLE + 4000) == Requester::CtlState::PAUSED && r.multi_flags() == -1 &&
              r.last_written() == Requester::NOT_WRITTEN);
        i += 510;  // 10.2 s later
        hear_sync(r, s + (i - 1) * CYCLE);
        CHECK(play_cycle(r, s + i * CYCLE, multi, p) == 0x05);
        r.target(true, 10, 4);
        i++;
        CHECK(play_cycle(r, s + i * CYCLE, multi, p) == 0x21 && p[5] == 0x05);  // the unit check
        bool read_first = false, wrote = false;
        for (int j = 0; j < 400 && !wrote; j++, i++) {
            uint8_t c = play_cycle(r, s + i * CYCLE, multi, p);
            if (c == 0x30 && (p[3] == 132 || p[4] == 132)) read_first = true;
            if (c == 0x37 && p[4] == 0x84) {
                wrote = true;
                CHECK(read_first && p[5] == 4);
            }
        }
        CHECK(wrote);
    }

    // Reading every setting: the serial number and firmware, then GetSettingInfo and ReadSetting for
    // each setting; an empty info reply marks a setting unsupported, three unanswered tries a failure.
    // It shares the cycles with the polls, and with another master on the bus it goes on in the cycles
    // that master leaves free, never in one it has claimed.
    {
        Requester r;
        int64_t t0 = 600000000;
        CHECK(!r.read_settings(t0));  // the controller is off
        r.control(true);
        hear_sync(r, t0);
        int64_t s = t0 + 151 * CYCLE;
        hear_sync(r, s - CYCLE);
        CHECK(r.read_settings(s));
        CHECK(!r.read_settings(s));  // one at a time
        CHECK(r.take_report(text, sizeof text) && strstr(text, "# settings: reading the serial number"));
        drop_info_for = 40;
        drops_left = 3;
        int i = 0, polls = 0;
        uint8_t p[16];
        for (; i < 3000 && !r.settings().done; i++) {
            uint8_t c = play_cycle(r, s + i * CYCLE, 4, p);
            if (c == 0x21 || c == 0x38 || (c == 0x30 && p[3] == 0 && p[4] == 13)) polls++;
            if (i % 50 == 49) drain(r);
        }
        const Requester::SettingsRead &rs = r.settings();
        CHECK(rs.done && !rs.active);
        CHECK(!strcmp(rs.serial, "HQ22514AWYY") && rs.firmware == 2623563);
        CHECK(rs.unsupported == 7 && rs.failed == 1 && rs.read == Requester::SETTINGS - 8);
        CHECK(rs.s[2].state == Requester::READ_OK && rs.s[2].value == 6 && rs.s[2].scale == -100 &&
              rs.s[2].offset == 0 && rs.s[2].def == 20 && rs.s[2].min == 0 && rs.s[2].max == 65535 &&
              rs.s[2].access == 0x80);
        CHECK(rs.s[191].state == Requester::READ_OK && rs.s[191].value == 573);
        CHECK(rs.s[13].state == Requester::UNSUPPORTED && rs.s[80].state == Requester::UNSUPPORTED);
        CHECK(rs.s[40].state == Requester::READ_FAILED);
        // the rest of the backup file: the unit, 0x3C's two replies, the records up to the Multi's 0x80
        CHECK(rs.unit_ok && rs.unit_id[0] == 0x48 && rs.unit_id[3] == 0x7A && rs.unit_info[0] == 0x2F &&
              rs.unit_info[8] == 0xE0);
        CHECK(rs.version_ok && rs.version[0] == 0x4B && rs.version[9] == 0x10);
        CHECK(rs.block_got[0] == 3 && rs.block_got[4] == 3 && rs.block[2][5] == 0x80 && rs.block[4][15] == 0x53);
        CHECK(rs.grid.ended && rs.grid.count == 3 && rs.grid.len[2] == 64 && rs.grid.data[1][63] == 127);
        CHECK(rs.assistants.ended && rs.assistants.count == 3 && rs.assistants.len[0] == 64 &&
              rs.assistants.len[1] == 0 && rs.assistants.len[2] == 6 && rs.assistants.data[2][5] == (uint8_t)(128 + 5));
        CHECK(!rs.problem && Requester::progress(rs) == 100);
        CHECK(polls > 20);  // the dashboard's polls went on meanwhile
        CHECK(r.take_report(text, sizeof text) && strstr(text, "# settings: 184 read, 7 not supported, 1 failed"));
        // again with an MK3 that claims every other cycle
        CHECK(r.read_settings(s + i * CYCLE));
        int claimed_sends = 0, sends = 0;
        bool claimed = false;
        for (int j = 0; j < 4000 && !r.settings().done; j++, i++) {
            int64_t c = s + i * CYCLE;
            uint8_t cmd = play_cycle(r, c, 4, p);
            if (cmd) sends++;
            if (claimed && cmd) claimed_sends++;
            claimed = j % 2 == 0;
            if (claimed) {  // an MK3 request after this cycle's cutoff claims the next cycle
                size_t k = mk3_frame(g);
                r.heard(g, k, c + 19300);
            }
            if (j % 50 == 49) drain(r);
        }
        CHECK(r.control_state(s + i * CYCLE) == Requester::CtlState::PAUSED);
        CHECK(r.settings().done && r.settings().read == Requester::SETTINGS - 7 && claimed_sends == 0);
        CHECK(sends > Requester::SETTINGS);
    }

    // The controller backs off for 30 s after three failed requests; a test run suspends it.
    {
        Requester r;
        int64_t t0 = 400000000;
        r.control(true);
        hear_sync(r, t0);
        int64_t s = t0 + 501 * CYCLE;
        for (int i = 0; i < 3; i++) {
            int64_t ts = s + i * 5 * CYCLE;
            hear_cycle(r, ts);
            CHECK(send(r, ts + 700, f) > 0);
            size_t k = e4_frame(g, f[3]);  // repeats our number: lost
            r.heard(g, k, ts + 15030);
            r.tick(ts + 250000);
        }
        CHECK(r.ctl_stats().failed == 3 && r.ctl_stats().backoffs == 1);
        CHECK(r.control_state(s + 15 * CYCLE) == Requester::CtlState::BACKING_OFF);
        CHECK(r.take_report(text, sizeof text) && strstr(text, "backing off for 30 s: 3 requests failed"));
        int64_t s2 = s + 10 * CYCLE + 250000 + 30000000 + CYCLE;
        hear_cycle(r, s2);
        CHECK(r.due(s2 + 700, f, sizeof f, &n, &earliest, &latest));
        r.start(s2 + 800, 10, Mode::READ);
        CHECK(r.control_state(s2 + 800) == Requester::CtlState::TESTING);
        hear_cycle(r, s2 + 60 * CYCLE);
        CHECK(r.due(s2 + 60 * CYCLE + 700, f, sizeof f, &n, &earliest, &latest) && f[6] == 0x30 && f[7] == 0x0D);
    }

    // The Multi's frame after a UART break (00 FB xx yy FF, xx + yy = 0x7F, heard at the assistant flag
    // change and after its restart) is not another master: the controller keeps polling and the next
    // cycle is not left. A garbled frame of any other shape still may be one.
    {
        Requester r;
        int64_t t0 = 450000000;
        r.control(true);
        hear_sync(r, t0);
        int64_t s = t0 + 501 * CYCLE;
        hear_cycle(r, s);
        CHECK(r.control_state(s) == Requester::CtlState::POLLING);
        const uint8_t brk[] = {0x00, 0xFB, 0x30, 0x4F, 0xFF}, brk2[] = {0x00, 0xFB, 0x50, 0x2F, 0xFF},
                      odd[] = {0x00, 0xFB, 0x30, 0x4E, 0xFF};
        CHECK(vebus::multi_break_frame(brk, sizeof brk) && vebus::multi_break_frame(brk2, sizeof brk2) &&
              !vebus::multi_break_frame(odd, sizeof odd));
        r.heard(brk, sizeof brk, s + 19300);  // late in the cycle, where a request would claim the next one
        CHECK(r.control_state(s + 19400) == Requester::CtlState::POLLING);
        hear_sync(r, s + CYCLE);
        CHECK(r.due(s + CYCLE + 700, f, sizeof f, &n, &earliest, &latest));  // not left to anyone
        CHECK(r.ctl_stats().yielded == 0);
        r.heard(odd, sizeof odd, s + CYCLE + 19300);
        CHECK(r.control_state(s + CYCLE + 19400) == Requester::CtlState::PAUSED);
    }

    // Venus SOC and external-current broadcasts: no ACK expected, companion
    // current immediately after SOC, and the own counter remains fed when PACE stops.
    {
        Requester r;r.control(true);
        int64_t t0=500000000,s=t0+501*CYCLE;hear_sync(r,t0);hear_cycle(r,s);
        verify_firmware(r,s);s+=CYCLE;r.target(true,10,-1);r.external_battery(true,61,12.3f);
        uint8_t p[16];int soc_count=0,current_count=0;bool companion=false;int64_t first_soc=-1,last_soc=-1;
        for(int i=0;i<1250;i++){
            memset(p,0,sizeof p);play_cycle(r,s+i*CYCLE,4,p);
            if(p[0]==0x38&&p[1]==4){
                CHECK(p[2]==122&&p[3]==0&&p[4]==0);soc_count++;companion=true;
                if(first_soc<0)first_soc=s+i*CYCLE;else CHECK(s+i*CYCLE-last_soc>=10000000);
                last_soc=s+i*CYCLE;
            }else if(p[0]==0x38&&p[1]==5){CHECK(p[2]==123&&p[3]==0&&p[4]==0);current_count++;companion=false;}
            else if(companion)CHECK(p[0]==0); // no regular poll between SOC and its companion
            if(i%40==0)drain(r);
        }
        CHECK(soc_count==3&&current_count>=9&&!companion&&r.ctl_stats().backoffs==0);
        auto before=r.soc_sync();r.external_battery(false,61,0);
        for(int i=1250;i<1850;i++){
            memset(p,0,sizeof p);play_cycle(r,s+i*CYCLE,4,p);
            CHECK(!(p[0]==0x38&&p[1]==4));
            if(p[0]==0x38&&p[1]==5)CHECK(p[2]==0&&p[3]==0&&p[4]==0);
            if(i%40==0)drain(r);
        }
        CHECK(r.soc_sync().soc_sent==before.soc_sent&&r.soc_sync().current_sent>before.current_sent);
        // Transmitted SOC cannot be treated as inverter acceptance/readback.
        vebus::Decoder d;const uint8_t socp[]={0x38,4,122,0,0};
        size_t n=vebus::build_frame(0x98,0xF7,0xFE,nr++,socp,sizeof socp,g,sizeof g);
        d.feed(g,n,1000,true);CHECK(d.state().soc.t<0);
    }

    // Stopping ESS after SOC still releases its estimator with one zero-current
    // companion. A foreign master defers that release until ownership is safe.
    for(bool master:{false,true})for(bool after_current:{false,true}){
        Requester r;r.control(true);
        const int64_t t0=560000000;int64_t s=t0+501*CYCLE;
        hear_sync(r,t0);hear_cycle(r,s);verify_firmware(r,s);s+=CYCLE;
        r.target(true,10,-1);r.external_battery(true,61,12.3f);
        uint8_t p[16]={};bool saw_soc=false,saw_current=false;
        for(int i=0;i<200&&!saw_current;i++,s+=CYCLE){
            memset(p,0,sizeof p);play_cycle(r,s,4,p);
            if(p[0]==0x38&&p[1]==4){saw_soc=true;if(!after_current){s+=CYCLE;break;}}
            if(p[0]==0x38&&p[1]==5&&saw_soc)saw_current=true;
        }
        CHECK(saw_soc&&(!after_current||saw_current));
        const auto writes=r.ctl_stats().writes;
        r.target(false,0,-1);r.external_battery(false,0,0);
        if(master){uint8_t f[64];const size_t n=mk3_frame(f);r.heard(f,n,s-300);}
        int released=0;
        for(int i=0;i<650;i++){
            memset(p,0,sizeof p);play_cycle(r,s+i*CYCLE,4,p);
            CHECK(!(p[0]==0x38&&p[1]==4));
            if(p[0]==0x38&&p[1]==5){
                CHECK(p[2]==0&&p[3]==0&&p[4]==0);released++;
                if(master)CHECK(i*CYCLE>=10000000-300);
            }
            if(i%40==0)drain(r);
        }
        CHECK(released==1&&r.ctl_stats().writes==writes);
        CHECK(r.control_state(s+649*CYCLE+19000)==Requester::CtlState::POLLING);
    }

    // A late reply from the old bus session must not authorize the next one.
    {
        Requester r;
        r.control(true);
        const int64_t s = 650000000;
        hear_sync(r, s - 4000000);
        hear_cycle(r, s);
        const uint8_t reset[] = {0x00, 0xFB, 0x30, 0x4F, 0xFF};
        CHECK(send(r, s + 700, f) == 9 && f[6] == 0x05);
        CHECK(next_counts(r, s + 15030));
        r.heard(reset, sizeof reset, s + 15500);
        hear_version(r, f[5], s + 16080);
        CHECK(r.firmware_gate(s + 17000));
        hear_sync(r, s + CYCLE);
        verify_firmware(r, s + CYCLE);
        r.target(true, 10, 6);
        hear_sync(r, s + 2 * CYCLE);
        CHECK(send(r, s + 2 * CYCLE + 700, f) > 0 && f[4] == 0x21 && f[9] == 0x05);
        CHECK(next_counts(r, s + 2 * CYCLE + 15030));
        r.heard(reset, sizeof reset, s + 2 * CYCLE + 15500);
        uint8_t unit[16];
        const size_t k = vebus::build_frame(0x83, 0x83, 0xFE, nr++, unit, assistant_unit_reply(unit), g, sizeof g);
        r.heard(g, k, s + 2 * CYCLE + 16080);
        CHECK(r.firmware_gate(s + 2 * CYCLE + 17000));
        hear_sync(r, s + 3 * CYCLE);
        verify_firmware(r, s + 3 * CYCLE);
        CHECK(r.control_state(s + 3 * CYCLE + 17000) == Requester::CtlState::POLLING);
        r.target(true, 10, 6);
        hear_sync(r, s + 4 * CYCLE);
        CHECK(send(r, s + 4 * CYCLE + 700, f) > 0 && f[4] == 0x21 && f[9] == 0x05);
        CHECK(r.ctl_stats().writes == 0 && r.ctl_stats().flag_writes == 0);
    }

    // Recognizing the product family must not allow a different revision or model to write.
    const uint32_t monitor_only[] = {2605558, 2615559, 2623556, 2636570, 2780556};
    for (uint32_t firmware : monitor_only) {
        Requester r;
        r.control(true);
        const int64_t t = 700000000;
        hear_sync(r, t - 4000000);
        hear_cycle(r, t - CYCLE);
        CHECK(play_cycle(r, t, 4, nullptr, firmware) == 0x05);
        CHECK(r.firmware_gate(t + 19000));
        char why[96];
        CHECK(!r.set_charger(t + 19000, charger::Change(), why, sizeof why));
        r.target(true, 300, 0);
        r.external_battery(true, 60, 10);
        bool saw_l1 = false;
        for (int i = 1; i <= 200; ++i) {
            uint8_t p[16] = {};
            if (!play_cycle(r, t + i * CYCLE, 4, p, firmware)) continue;
            CHECK(p[0] != 0x38 && p[0] != 0x3F);
            CHECK(p[0] != 0x00 || (p[2] != 0x32 && p[2] != 0x33 && p[2] != 0x34 && p[2] != 0x37));
            if (p[0] == 0x21 && p[5] == 0x08 && p[6] == 0x00) saw_l1 = true;
        }
        CHECK(saw_l1);
        CHECK(r.ctl_stats().writes == 0 && r.ctl_stats().flag_writes == 0);
    }

    test_assistant_discovery();
    test_48_3000_ess();
    test_mp2_ess_profile(2611552, 0x3F, 137);
    test_mp2_ess_profile(2601560, 0x42, 137);
    test_read_only_assistant_discovery();
    test_settings_inspection();
    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
