#include "battery_bms.h"
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

using namespace battery_bms;
namespace {
using JkFrame = std::array<uint8_t, 300>;
using Packet = std::array<uint8_t, 20>;
constexpr int64_t T = 100000000;
constexpr uint32_t MS = 100000;
void put16(uint8_t *p, uint16_t value) { p[0] = uint8_t(value); p[1] = uint8_t(value >> 8); }
void put32(uint8_t *p, uint32_t value) { put16(p, uint16_t(value)); put16(p + 2, uint16_t(value >> 16)); }
void checksum(JkFrame &f) { f[299] = 0; for (size_t i = 0; i < 299; ++i) f[299] = uint8_t(f[299] + f[i]); }
bool near(float a, float b, float epsilon = .0002f) { return std::fabs(a - b) < epsilon; }
bool contains(const void *memory, size_t length, const char *needle) {
    const auto *p = static_cast<const uint8_t *>(memory); const size_t n = std::strlen(needle);
    for (size_t i = 0; i + n <= length; ++i) if (!std::memcmp(p + i, needle, n)) return true;
    return false;
}
JkFrame identity(const char *hardware = "10.XG", const char *firmware = "10.07") {
    JkFrame f{}; f[0] = 0x55; f[1] = 0xaa; f[2] = 0xeb; f[3] = 0x90; f[4] = 3; f[5] = 7;
    std::memcpy(f.data() + 6, "JK-B2A24S20P", 13);
    std::memcpy(f.data() + 22, hardware, std::strlen(hardware));
    std::memcpy(f.data() + 30, firmware, std::strlen(firmware));
    // Deliberately synthetic secrets: no private recorded BMS file is a fixture.
    std::memcpy(f.data() + 62, "secret-passcode", 15);
    std::memcpy(f.data() + 86, "secret-serial", 13);
    std::memcpy(f.data() + 102, "secret-userdata", 15);
    std::memcpy(f.data() + 118, "secret-setup", 12);
    checksum(f); return f;
}
JkFrame status(uint8_t counter = 1, int32_t current_ma = -12000) {
    JkFrame f{}; f[0] = 0x55; f[1] = 0xaa; f[2] = 0xeb; f[3] = 0x90; f[4] = 2; f[5] = counter;
    uint32_t total_mv = 0;
    for (unsigned i = 0; i < 16; ++i) {
        const uint16_t value = i == 0 ? 3270 : i == 15 ? 3300 : 3280;
        total_mv += value; put16(f.data() + 6 + 2 * i, value);
    }
    put32(f.data() + 54, 0xffff); put32(f.data() + 118, total_mv);
    put32(f.data() + 126, uint32_t(current_ma));
    put16(f.data() + 130, uint16_t(-50)); put16(f.data() + 132, 273); put16(f.data() + 134, 312);
    put16(f.data() + 136, 0x800); put16(f.data() + 138, uint16_t(-120)); f[140] = 2;
    f[141] = 56; put32(f.data() + 142, 128800); put32(f.data() + 146, 230000);
    put32(f.data() + 150, 481); f[158] = 98; f[166] = 1; f[167] = 1;
    checksum(f); return f;
}
JkFeed jk_ready() {
    JkFeed f; f.connected(true); const auto info = identity(); assert(f.accept(info.data(), info.size(), T));
    assert(f.profile_ready()); return f;
}
Packet metadata(uint32_t tick = MS, uint32_t boot = 0x12345678) {
    Packet m{}; std::memcpy(m.data(), "BZCB", 4); m[4] = 1; m[5] = 1;
    put32(m.data() + 6, boot); put32(m.data() + 10, tick); put32(m.data() + 14, 100); return m;
}
Packet can(uint16_t id, uint32_t sequence, uint32_t tick = MS + 30) {
    Packet f{}; f[0] = 1; f[1] = 8; put16(f.data() + 2, id); put32(f.data() + 4, sequence); put32(f.data() + 8, tick);
    uint8_t *p = f.data() + 12;
    switch (id) {
    case 0x351: put16(p, 577); put16(p + 2, 1450); put16(p + 4, 1550); break;
    case 0x355: put16(p, 63); put16(p + 2, 99); break;
    case 0x356: put16(p, 5240); put16(p + 2, uint16_t(-133)); put16(p + 4, 292); break;
    case 0x359: p[0] = 0x84; p[1] = 8; p[2] = 0x10; p[3] = 1; p[4] = 1; break;
    case 0x35c: p[0] = 0xf8; break;
    case 0x35e: std::memcpy(p, "PYLON   ", 8); break;
    case 0x373: put16(p, 3260); put16(p + 2, 3290); put16(p + 4, 300); put16(p + 6, 303); break;
    }
    return f;
}
PaceFeed pace_ready(uint32_t tick = MS) {
    PaceFeed f; f.connected(true); const auto m = metadata(tick); assert(f.anchor(m.data(), m.size(), T, T + 20000));
    return f;
}
void fill_pace(PaceFeed &f) {
    const uint16_t ids[] = {0x351, 0x355, 0x356, 0x359, 0x35c, 0x35e, 0x373};
    uint32_t sequence = 1;
    for (auto id : ids) { const auto frame = can(id, sequence++); assert(f.accept(frame.data(), frame.size(), T + 60000)); }
}
}

int main() {
    static_assert(sizeof(JkFeed) < 768, "JK must use bounded, small storage");
    static_assert(sizeof(PaceFeed) < 512, "PACE cache must stay small");
    static_assert(sizeof(Snapshot) < 320, "Snapshot must be bounded");
    {
        uint8_t request[20]; assert(JkFeed::request(0x97, request));
        const uint8_t expected[] = {0xaa, 0x55, 0x90, 0xeb, 0x97, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x11};
        assert(!std::memcmp(request, expected, 20)); assert(JkFeed::request(0x96, request)); assert(request[19] == 0x10);
        for (unsigned op = 0; op < 256; ++op) {
            if (op == 0x96 || op == 0x97) continue;
            std::memset(request, 0xff, 20); assert(!JkFeed::request(uint8_t(op), request));
            for (auto byte : request) assert(!byte);
        }
        assert(!JkFeed::request(0x97, nullptr));
    }
    {
        JkFeed f; auto info = identity(); auto s = status();
        assert(!f.accept(info.data(), info.size(), T)); f.connected(true);
        assert(!f.accept(s.data(), s.size(), T)); assert(!f.view(T).valid);
        assert(f.accept(info.data(), info.size(), T)); assert(f.profile_ready());
        assert(!contains(&f, sizeof f, "secret-"));
        const auto id = f.view(T); assert(id.has(IDENTITY)); assert(!std::strcmp(id.model, "JK-B2A24S20P"));
        assert(!std::strcmp(id.hardware, "10.XG")); assert(!std::strcmp(id.firmware, "10.07"));
        uint8_t tailed_identity[320]{}; std::memcpy(tailed_identity, info.data(), info.size());
        const uint8_t opaque_tail[] = {0x55, 0xaa, 0xeb, 0x90, 's', 'e', 'c', 'r', 'e', 't', '-', 't', 'a', 'i', 'l'};
        std::memcpy(tailed_identity + 300, opaque_tail, sizeof opaque_tail);
        assert(f.accept(tailed_identity, sizeof tailed_identity, T));
        assert(!contains(&f, sizeof f, "secret-"));
        assert(f.accept(s.data(), s.size(), T + 1)); auto v = f.view(T + 1);
        assert(v.complete && v.connected && v.has(SOC | VOLTAGE | CURRENT | CELL_EXTREMA | CELL_VALUES));
        assert(near(v.soc, 56) && near(v.voltage, 52.49f) && near(v.current, -12));
        assert(v.cell_count == 16 && v.min_cell_index == 1 && v.max_cell_index == 16);
        assert(near(v.min_cell_v, 3.27f) && near(v.max_cell_v, 3.3f) && near(v.delta_cell_v, .03f));
        assert(v.has(CAPACITY | REMAINING | SOH | TEMPERATURES | MOS_TEMPERATURE | PERMISSIONS | ALARMS | BALANCING | CYCLES));
        assert(near(v.capacity_ah, 230) && near(v.remaining_ah, 128.8f) && v.cycles == 481);
        assert(near(v.temp_min_c, -5) && near(v.temp_max_c, 27.3f) && near(v.temp_mos_c, 31.2f));
        assert(v.charge_allowed && v.discharge_allowed && v.balancing && near(v.balancing_current_a, -.12f));
        assert(v.alarms == 0x800 && !v.warnings && !v.has(LIMITS));
        assert(f.view(T + FRESH_US).has(SOC)); assert(!f.view(T + FRESH_US + 1).has(SOC));
        assert(f.view(T).valid == IDENTITY); // future observations are never fresh
        f.connected(false); assert(!f.view(T + 2).valid && !f.profile_ready());
        assert(!contains(&f, sizeof f, "secret-"));
        f.connected(true); assert(!f.profile_ready() && !f.view(T + 2).valid);
    }
    // Every possible two-fragment boundary, including split headers; trailers
    // and records containing a header-like payload remain bounded.
    for (size_t split = 1; split < 300; ++split) {
        JkFeed f; f.connected(true); const auto info = identity();
        assert(!f.accept(info.data(), split, T)); assert(f.accept(info.data() + split, info.size() - split, T + 1));
        assert(f.profile_ready() && !contains(&f, sizeof f, "secret-"));
        const auto s = status(); assert(!f.accept(s.data(), split, T + 2));
        assert(f.accept(s.data() + split, s.size() - split, T + 3)); assert(f.view(T + 3).complete);
    }
    {
        JkFeed f; f.connected(true); const auto info = identity();
        for (size_t i = 0; i < info.size(); ++i) assert(f.accept(info.data() + i, 1, T + int64_t(i)) == (i == 299));
        assert(f.profile_ready()); auto s = status(); uint8_t trailer[320]{}; std::memcpy(trailer, s.data(), 300);
        const uint8_t ending[] = {0xaa, 0x55, 0x90, 0xeb}; std::memcpy(trailer + 300, ending, 4);
        assert(f.accept(trailer, sizeof trailer, T + 1000)); assert(f.view(T + 1000).has(SOC));
        s = status(2); const uint8_t noise[] = {0, 0x55, 0x55}; assert(!f.accept(noise, sizeof noise, T + 2000));
        assert(f.accept(s.data() + 1, s.size() - 1, T + 2001)); // retained last55 starts the header
        s = status(3); assert(!f.accept(s.data(), 128, T + 3000));
        assert(!f.accept(s.data() + 128, s.size() - 128, T + 3000 + 2000001));
        assert(f.accept(s.data(), s.size(), T + 3000 + 2000002));
    }
    {
        auto f = jk_ready(); auto s = status(255); assert(f.accept(s.data(), s.size(), T + 1));
        assert(!f.accept(s.data(), s.size(), T + 1000000)); assert(f.stats().duplicates == 1);
        s = status(0, 25000); assert(f.accept(s.data(), s.size(), T + 1000001)); assert(near(f.view(T + 1000001).current, 25));
        s = status(255); assert(!f.accept(s.data(), s.size(), T + 1000002));
        s = status(1); s[100] ^= 1; assert(!f.accept(s.data(), s.size(), T + 2000000)); assert(f.stats().checksum_errors == 1);
        assert(near(f.view(T + 2000000).current, 25));
        assert(!f.accept(nullptr, 300, T)); assert(!f.accept(s.data(), 0, T));
        uint8_t too_long[401]{}; assert(!f.accept(too_long, sizeof too_long, T));
        s = identity(); s[4] = 1; checksum(s); assert(!f.accept(s.data(), s.size(), T + 2000001));
        assert(!contains(&f, sizeof f, "secret-"));
    }
    {
        auto f = jk_ready(); auto s = status(); s[141] = 255; put32(s.data() + 126, 0x7fffffff); checksum(s);
        assert(f.accept(s.data(), s.size(), T + 1)); auto v = f.view(T + 1);
        assert(!v.has(SOC) && !v.has(CURRENT) && v.has(VOLTAGE | CELL_EXTREMA));
        s = status(2); put16(s.data() + 134, 0x7fff); s[166] = 255; checksum(s);
        assert(f.accept(s.data(), s.size(), T + 2)); v = f.view(T + 2);
        assert(!v.has(MOS_TEMPERATURE) && !v.has(PERMISSIONS) && v.has(SOC | CURRENT));
        s = status(3); put32(s.data() + 54, 0xffffffff); checksum(s);
        assert(!f.accept(s.data(), s.size(), T + 3)); assert(f.view(T + 3).valid == IDENTITY);
        s = status(3); put32(s.data() + 118, 12000); checksum(s);
        assert(!f.accept(s.data(), s.size(), T + 4));
        s = status(3); assert(f.accept(s.data(), s.size(), T + 5));
        const auto unsupported = identity("11.XW", "11.26"); assert(f.accept(unsupported.data(), unsupported.size(), T + 6));
        assert(!f.profile_ready()); assert(f.view(T + 6).valid == IDENTITY);
        assert(!f.accept(s.data(), s.size(), T + 7));
        auto invalid_id = identity(); invalid_id[6] = 0xff; checksum(invalid_id);
        assert(!f.accept(invalid_id.data(), invalid_id.size(), T + 8)); assert(!f.view(T + 8).valid);
    }
    {
        auto f = pace_ready(); fill_pace(f); auto v = f.view(T + 60000);
        assert(v.complete && v.has(SOC | SOH | VOLTAGE | CURRENT | PACK_TEMPERATURE | CELL_EXTREMA | TEMPERATURES | LIMITS | PERMISSIONS | ALARMS | IDENTITY));
        assert(!v.has(CAPACITY) && !v.has(CELL_VALUES));
        assert(near(v.soc, 63) && near(v.voltage, 52.4f) && near(v.current, -13.3f));
        assert(near(v.charge_voltage_limit_v, 57.7f) && near(v.charge_current_limit_a, 145) && near(v.discharge_current_limit_a, 155));
        assert(near(v.min_cell_v, 3.26f) && near(v.max_cell_v, 3.29f) && near(v.delta_cell_v, .03f));
        assert(near(v.temp_min_c, 26.85f) && near(v.temp_max_c, 29.85f) && near(v.temperature_c, 29.2f));
        assert(v.charge_allowed && v.discharge_allowed && v.force_charge && v.full_charge_requested);
        assert(v.alarms == 0x884 && v.warnings == 0x110 && v.module_count == 1);
        assert(!std::strcmp(v.protocol, "PYLON   "));
        assert(v.sample_age_s > .28f && v.sample_age_s < .283f); // source uncertainty is included
        auto recent_v = can(0x356, 8, MS + 9030); assert(f.accept(recent_v.data(), recent_v.size(), T + 9060000));
        v = f.view(T + 10060000); assert(v.has(VOLTAGE | CURRENT | PACK_TEMPERATURE));
        assert(!v.has(SOC) && !v.has(LIMITS) && !v.has(PERMISSIONS) && !v.has(ALARMS) && !v.has(CELL_EXTREMA) && !v.has(TEMPERATURES));
        const auto m = metadata(MS + 10000); assert(f.anchor(m.data(), m.size(), T + 10000000, T + 10020000));
        assert(!f.view(T + 10060000).has(LIMITS)); // metadata cannot refresh a CAN ID
        assert(!f.view(T + 10060000).has(SOC));
        f.connected(false); assert(!f.view(T + 10060000).valid && !f.anchored());
        f.connected(true); assert(!f.anchored() && !f.view(T + 10060000).valid);
    }
    {
        auto f = pace_ready(); auto frame = can(0x355, 0xffffffffu);
        assert(f.accept(frame.data(), frame.size(), T + 60000));
        assert(!f.accept(frame.data(), frame.size(), T + 1000000)); assert(f.stats().duplicates == 1);
        frame = can(0x355, 0, MS + 1000); assert(f.accept(frame.data(), frame.size(), T + 1020000));
        frame = can(0x355, 0x80000000u, MS + 2000); assert(!f.accept(frame.data(), frame.size(), T + 2020000));
        frame = can(0x355, 1, MS + 1000); assert(f.accept(frame.data(), frame.size(), T + 3020000));
        assert(f.view(T + 3020000).soc_age_s > 2.27f); // new seq with old source time does not refresh
        assert(!f.view(T + 11000000).has(SOC));
        frame = can(0x355, 2, MS + 100000); assert(!f.accept(frame.data(), frame.size(), T + 2000000));
        frame = can(0x355, 2, MS - 9900); assert(!f.accept(frame.data(), frame.size(), T + 20000));
        assert(!f.accept(nullptr, 20, T + 20000)); assert(!f.accept(frame.data(), 19, T + 20000));
        frame = can(0x400, 2); assert(!f.accept(frame.data(), frame.size(), T + 60000));
        frame = can(0x355, 2); frame[0] = 2; assert(!f.accept(frame.data(), frame.size(), T + 60000));
        frame[0] = 1; frame[1] = 9; assert(!f.accept(frame.data(), frame.size(), T + 60000));
        frame[1] = 4; frame[19] = 1; assert(!f.accept(frame.data(), frame.size(), T + 60000));
        frame[19] = 0; assert(f.accept(frame.data(), frame.size(), T + 60000) == false); // source-time regression
    }
    {
        auto f = pace_ready(0xfffffff0u); auto frame = can(0x355, 1, 0x10);
        assert(f.accept(frame.data(), frame.size(), T + 62000)); assert(f.view(T + 62000).has(SOC));
        const auto meta_wrap = metadata(0x3e8); assert(f.anchor(meta_wrap.data(), meta_wrap.size(), T + 1010000, T + 1030000));
        assert(f.view(T + 1030000).has(SOC));
        auto reboot = metadata(10, 0x98765432); assert(f.anchor(reboot.data(), reboot.size(), T + 1040000, T + 1060000));
        assert(!f.view(T + 1060000).valid); assert(f.boot_id() == 0x98765432);
        frame = can(0x355, 1, 40); assert(f.accept(frame.data(), frame.size(), T + 1100000));
        assert(f.view(T + 1100000).has(SOC));
        auto stale_meta = metadata(10, 0x98765432); assert(!f.anchor(stale_meta.data(), stale_meta.size(), T + 2040000, T + 2060000));
        assert(!f.anchored() && !f.view(T + 2060000).valid);
    }
    {
        PaceFeed f; auto m = metadata(); assert(!f.anchor(m.data(), m.size(), T, T + 1000)); f.connected(true);
        assert(!f.anchor(m.data(), m.size(), T, T + 250001));
        assert(f.anchor(m.data(), m.size(), T, T + 250000));
        m[5] = 3; assert(!f.anchor(m.data(), m.size(), T + 300000, T + 310000)); assert(!f.anchored());
        m[5] = 1; m[0] = 'X'; assert(!f.anchor(m.data(), m.size(), T, T + 1000));
        m = metadata(); assert(!f.anchor(m.data(), 19, T, T + 1000));
        assert(!f.anchor(m.data(), m.size(), T, T - 1));
        assert(f.anchor(m.data(), m.size(), T, T + 1000));
        auto jump = metadata(MS + 50000); assert(!f.anchor(jump.data(), jump.size(), T + 1000000, T + 1010000));
        assert(!f.anchored());
    }
    // Invalid fields do not keep older limits/permissions or alarm-clear flags alive.
    {
        auto f = pace_ready(); fill_pace(f);
        auto bad = can(0x351, 8, MS + 100); put16(bad.data() + 14, 0xffff);
        assert(!f.accept(bad.data(), bad.size(), T + 120000)); assert(!f.view(T + 120000).has(LIMITS));
        auto v = can(0x356, 9, MS + 100); assert(f.accept(v.data(), v.size(), T + 120000));
        bad = can(0x35c, 10, MS + 110); bad[12] = 0xc1;
        assert(!f.accept(bad.data(), bad.size(), T + 130000)); assert(!f.view(T + 130000).has(PERMISSIONS));
        bad = can(0x359, 11, MS + 110); bad[16] = 0;
        assert(!f.accept(bad.data(), bad.size(), T + 130000)); assert(!f.view(T + 130000).has(ALARMS));
        bad = can(0x373, 12, MS + 110); put16(bad.data() + 12, 4000); put16(bad.data() + 14, 3000);
        assert(!f.accept(bad.data(), bad.size(), T + 130000)); assert(!f.view(T + 130000).has(CELL_EXTREMA));
        bad = can(0x373, 12, MS + 110); put16(bad.data() + 16, 0);
        assert(f.accept(bad.data(), bad.size(), T + 130000));
        assert(f.view(T + 130000).has(CELL_EXTREMA) && !f.view(T + 130000).has(TEMPERATURES));
        v = can(0x356, 13, MS + 120); assert(f.accept(v.data(), v.size(), T + 140000));
        assert(!f.view(T + 140000).has(TEMPERATURES) && f.view(T + 140000).has(PACK_TEMPERATURE));
        bad = can(0x35e, 14, MS + 130); bad[12] = 0xff;
        assert(!f.accept(bad.data(), bad.size(), T + 150000)); assert(!f.view(T + 150000).has(IDENTITY));
    }
    {
        auto j = jk_ready(); const auto s = status(); assert(j.accept(s.data(), s.size(), T + 60000));
        auto p = pace_ready(); fill_pace(p); const auto a = j.view(T + 60000), b = p.view(T + 60000);
        auto bank = aggregate(a, b); assert(!bank.has(SOC | CAPACITY)); assert(bank.has(VOLTAGE | CURRENT));
        bank = aggregate(a, b, 280); assert(bank.complete && bank.has(SOC | CAPACITY | CURRENT | CELL_EXTREMA));
        assert(near(bank.soc, (56.f * 230.f + 63.f * 280.f) / 510.f)); assert(near(bank.capacity_ah, 510));
        assert(near(bank.current, -25.3f) && near(bank.voltage, 52.445f));
        assert(near(bank.min_cell_v, 3.26f) && near(bank.max_cell_v, 3.3f) && near(bank.delta_cell_v, .04f));
        assert(!bank.has(LIMITS) && !bank.has(CELL_VALUES));
        assert(!aggregate(a, b, std::numeric_limits<float>::quiet_NaN()).has(SOC));
        auto missing = b; missing.valid &= ~SOC; assert(!aggregate(a, missing, 280).has(SOC));
        missing = b; missing.connected = false; assert(!aggregate(a, missing, 280).valid);
        missing = b; missing.valid &= ~CELL_EXTREMA; assert(!aggregate(a, missing, 280).has(CELL_EXTREMA));
        missing = b; missing.voltage = 12.8f; assert(!aggregate(a, missing, 280).has(SOC | CURRENT | CELL_EXTREMA));
        missing = b; missing.charge_allowed = false; assert(!aggregate(a, missing, 280).charge_allowed);
        assert(!aggregate(j.view(T + FRESH_US + 60000), b, 280).complete);
    }
    std::printf("Battery protocol, per-ID freshness, source-clock, fragmentation, privacy and parallel-bank checks passed; sizes JK=%zu PACE=%zu snapshot=%zu\n",
                sizeof(JkFeed), sizeof(PaceFeed), sizeof(Snapshot));
}
