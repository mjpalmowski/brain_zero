// Synthetic legacy8 Assistant upload regression; no captured file or hardware is used.
#include <stdio.h>
#include <string.h>

#include <initializer_list>
#include <map>
#include <vector>

#include "requester.h"
#include "rvsc.h"
#include "upload.h"
#include "vebus_state.h"

using vebus::Requester;
typedef std::vector<uint8_t> Bytes;

static int failures = 0;
#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            printf("FAIL line %d: %s\n", __LINE__, #cond);             \
            failures++;                                                \
        }                                                              \
    } while (0)

static Bytes read_file(const char *path) {
    Bytes b;
    FILE *f = fopen(path, "rb");
    if (!f) return b;
    int c;
    while ((c = fgetc(f)) != EOF) b.push_back((uint8_t)c);
    fclose(f);
    return b;
}

static uint32_t u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

// Locate a section in the synthetic file.
static size_t section_payload(const Bytes &f, const char *name) {
    size_t pos = 31;
    while (pos + 2 < f.size()) {
        size_t len = f[pos] | f[pos + 1] << 8;
        if (len == strlen(name) && memcmp(&f[pos + 2], name, len) == 0) return pos + 2 + len + 4;
        pos = u32(&f[pos + 2 + len]);
    }
    return 0;
}

static size_t unit_payload(const Bytes &f) { return section_payload(f, "BareSettingData"); }

static void fix_checksums(Bytes &f) {
    size_t pos = 31;
    while (pos + 2 < f.size()) {
        size_t len = f[pos] | f[pos + 1] << 8;
        size_t next = u32(&f[pos + 2 + len]);
        uint32_t sum = 0;
        for (size_t i = pos; i < next - 4; i += 4) {
            uint32_t w = 0;
            for (size_t j = 0; j < 4 && i + j < next - 4; j++) w |= (uint32_t)f[i + j] << (8 * j);
            sum += w;
        }
        for (int i = 0; i < 4; i++) f[next - 4 + i] = (uint8_t)(sum >> (8 * i));
        pos = next;
    }
}

static size_t unit_info_offset(const Bytes &f) {
    const size_t payload = unit_payload(f);
    const bool legacy = u32(f.data() + payload) == 2;
    size_t at = payload + 12 + (legacy ? 1 : 3) + 4;
    const unsigned tail = f[at] | f[at + 1] << 8;
    return at + 2 + tail;
}

static size_t auth_offset(const Bytes &f) {
    const size_t at = unit_info_offset(f) + 10;
    const unsigned serial = f[at] | f[at + 1] << 8;
    return at + 2 + serial;
}

static size_t auth_end(const Bytes &f) {
    size_t at = auth_offset(f);
    const unsigned count = f[at] | f[at + 1] << 8;
    at += 2;
    for (unsigned i = 0; i < count; ++i) {
        const unsigned n = f[at] | f[at + 1] << 8;
        at += 2 + n;
    }
    return at + 16;
}

static size_t settings_offset(const Bytes &f) { return auth_end(f) + 2; }

static void set_setting(Bytes &f, int id, uint16_t v) {
    size_t at = settings_offset(f) + 2 * id;
    f[at] = (uint8_t)v;
    f[at + 1] = (uint8_t)(v >> 8);
    fix_checksums(f);
}

static const int64_t UTC = 1793613600;
// Synthetic credentials only; these files are never uploaded to the physical inverter.
static const uint8_t CREDENTIAL[8] = {0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7};
static Bytes credentials(Bytes f, unsigned count = 1, int64_t valid = UTC, int64_t saved = UTC,
                         bool correct = true, unsigned length = 8) {
    size_t payload = unit_payload(f), at = auth_offset(f), old_end = auth_end(f);
    const size_t old_size = old_end - at;
    Bytes header;
    auto put = [&](uint64_t value, unsigned n) {
        for (unsigned i = 0; i < n; i++) header.push_back((uint8_t)(value >> (8 * i)));
    };
    put(count, 2);
    for (unsigned i = 0; i < count; i++) {
        put(length, 2);
        for (unsigned k = 0; k < length; k++) header.push_back(CREDENTIAL[k % 8] ^ (correct ? 0 : 0x10));
    }
    put((uint64_t)valid, 8); put((uint64_t)saved, 8);
    f.erase(f.begin() + at, f.begin() + old_end);
    f.insert(f.begin() + at, header.begin(), header.end());
    size_t next_at = payload - 4;
    uint32_t next = u32(&f[next_at]) + (uint32_t)header.size() - (uint32_t)old_size;
    for (unsigned i = 0; i < 4; i++) f[next_at + i] = (uint8_t)(next >> (8 * i));
    fix_checksums(f);
    return f;
}

// Minimal synthetic telemetry RAM; this harness does not enable ESS controls.
static size_t assistant_ram_reply(uint8_t *out, const uint8_t *request, size_t size, uint16_t) {
    out[0] = 0; out[1] = request[1]; out[2] = 0x85;
    const size_t bytes = 3 + 2 * (size - 3);
    memset(out + 3, 0, bytes - 3);
    return bytes;
}

// ---------------------------------------------------------------- the simulated Multi

struct Multi {
    std::map<Bytes, std::vector<Bytes>> table;  // synthetic replies, for what does not change
    bool supported[Requester::SETTINGS] = {};
    uint16_t value[Requester::SETTINGS] = {};
    uint8_t mem[Requester::MAX_RECORDS * Requester::RECORD + 2];
    int records = 0;
    uint8_t unit_id[4] = {}, unit_info[10] = {};
    bool marked = true;   // the roll call's mark: unmarked units answer 21 .. 00
    int reset_in = 0;     // cycles until a reset asked for takes effect (1.1 s on the bus)
    int silent = 0;       // cycles without syncs after a reset
    int announce = 0;     // announcements still to send
    int resets = 0, identifies = 0;
    bool mk3 = false;     // an MK3 on the bus: a request after the cutoff every other cycle
    bool mk3_hello = false;  // an MK3 that greets the restarted Multi (8F F7 FC 0f 06 29), as on 2026-10-27
    uint8_t break_x = 0;  // a break frame due, 00 FB x 7F-x FF, as the Multi sends at the flag change and restart
    std::vector<Bytes> sent;  // every payload the ESP32 sent
    Bytes later;              // Winmon 0x3C's second reply, for the next cycle
    bool authorized = false, reject_auth = false, drop_auth = false;
    int auth_requests = 0, setting_attempts = 0, grid_queries = 0, program_writes = 0, unit_writes = 0;
    int deny_setting = -1, ignore_setting = -1;
    int deny_record = -1, drop_record = -1;
    bool ignore_final_mark = false, drop_last_read = false;

    void put_firmware(Bytes &frame, size_t offset) const {
        const Bytes &reply = table.at({0x05})[0];
        memcpy(frame.data() + offset, reply.data() + 1, 4);
    }

    bool protected_change(unsigned id, uint16_t v) const {
        auto it = table.find(Bytes{0x35, (uint8_t)id, 0});
        if (it != table.end() && !it->second.empty() && it->second[0].size() >= 12 &&
            (it->second[0][11] & 0x7F)) return true;
        it = table.find(Bytes{0x3C, (uint8_t)id, 0});
        if (it != table.end()) {
            for (unsigned bit = 0; bit < 16; bit++) {
                unsigned half = bit / 8, off = 2 + bit % 8;
                if (((value[id] ^ v) & (1u << bit)) && half < it->second.size() &&
                    off < it->second[half].size() && (it->second[half][off] & 0x7F)) return true;
            }
        }
        return false;
    }

    // Replay only the self-contained synthetic fixture's descriptors and values.
    void init(const rvsc::File &file) {
        memcpy(unit_id, file.unit_id, sizeof unit_id);
        memcpy(unit_info, file.unit_info, sizeof unit_info);
        records = file.assistant_bytes / Requester::RECORD;
        memcpy(mem, file.assistants, file.assistant_bytes);

        Bytes serial{0x9D}, version{0x82};
        serial.insert(serial.end(), file.serial, file.serial + strlen(file.serial));
        version.insert(version.end(), file.version, file.version + file.version_bytes);
        table[{0x3B}] = {serial};
        table[{0x05}] = {version};
        for (unsigned i = 0; i < Requester::SETTINGS; i++) {
            const uint8_t empty[10] = {};
            supported[i] = memcmp(file.records[i], empty, sizeof empty) != 0;
            value[i] = file.settings[i];
            Bytes info{0x89};
            if (supported[i]) {
                info.insert(info.end(), file.records[i], file.records[i] + sizeof empty);
                info.push_back(file.access[i]);
            }
            table[{0x35, uint8_t(i), 0}] = {info};
        }

        const uint8_t flag_ids[] = {0, 1, 60, 61, 82};
        for (unsigned i = 0; i < sizeof flag_ids; i++) {
            const uint8_t *access = file.block + i * Requester::BLOCK_BYTES;
            Bytes first{0x9E, flag_ids[i]}, second{0x9F, flag_ids[i]};
            first.insert(first.end(), access, access + 8);
            second.insert(second.end(), access + 8, access + 16);
            table[{0x3C, flag_ids[i], 0}] = {first, second};
        }

        int grid_at = 0;
        for (int record = 0; grid_at < file.grid_len; record++) {
            const int left = file.grid_len - grid_at;
            const int size = left < Requester::RECORD ? left : Requester::RECORD;
            for (int offset = 0, chunk = 1; chunk <= Requester::CHUNKS; chunk++, offset += Requester::CHUNK) {
                Bytes reply{0x80, 0x02};
                if (offset < size) {
                    const int available = size - offset;
                    const int count = available < Requester::CHUNK ? available : Requester::CHUNK;
                    const uint8_t *data = file.grid + grid_at + offset;
                    reply = {0xA3};
                    reply.insert(reply.end(), data, data + count);
                }
                table[{0x57, uint8_t(chunk), uint8_t(record)}] = {reply};
            }
            grid_at += size;
        }
        const int grid_records = (file.grid_len + Requester::RECORD - 1) / Requester::RECORD;
        if (file.grid_present) table[{0x57, 1, uint8_t(grid_records)}] = {{0x80, 0x02}};
    }
    bool blank(int r) const {
        for (int k = 0; k < Requester::RECORD; k++) {
            if (mem[r * Requester::RECORD + k] != 0xFF) return false;
        }
        return true;
    }
    // The replies to one request: the first comes in this cycle, a second (0x3C's) in the next.
    std::vector<Bytes> answer(const Bytes &p) {
        std::vector<Bytes> out;
        if (p.empty()) return out;
        if (p[0] == 0x21 && p.size() >= 6) {
            bool to_all = p[1] == 0 && p[2] == 0 && p[3] == 0 && p[4] == 0;
            bool to_unit = memcmp(&p[1], unit_id, 4) == 0;
            uint8_t sub = p[5];
            Bytes info;
            if (sub == 0x08 && p.size() >= 7) {  // DC or L1 info
                info = Bytes(15, 0); info[0] = 0x20;
            } else if (sub == 0x05 && (to_all || to_unit)) {
                info = {0x20};
                info.insert(info.end(), unit_id, unit_id + 4);
                info.insert(info.end(), unit_info, unit_info + 10);
            } else if (sub == 0x04 && to_unit && p.size() >= 15) {
                ++unit_writes;
                if (!ignore_final_mark || (p[13] >> 4) != 0x0E) memcpy(unit_info + 1, &p[6], 9);
                if ((p[13] >> 4) == 0x0F) break_x = 0x50;  // the assistants off: a break frame follows
            } else if (sub == 0x03 && to_all && reset_in == 0) {  // mk2vsc's second one comes before it
                authorized = false;
                resets++;
                reset_in = 55;
            } else if (sub == 0x01 && p.size() >= 7) {
                if (to_all && p[6] == 0x00) marked = false;
                if (to_unit && p[6] == 0x01) marked = true;
            } else if (sub == 0x00 && to_all && !marked) {
                identifies++;
                info = {0x20};
                info.insert(info.end(), unit_id, unit_id + 4);
                Bytes rest = {1, 0, 0, 0, 0, 0, 0xFF, 1};
                put_firmware(rest, 2);
                info.insert(info.end(), rest.begin(), rest.end());
            }
            if (!info.empty()) out.push_back(info);
            return out;
        }
        if (p[0] != 0x00 || p.size() < 3) return out;
        uint8_t id = p[1], cmd = p[2];
        if (cmd == 0x57) ++grid_queries;
        if (cmd >= 0x58 && cmd <= 0x5E) ++program_writes;
        auto reply = [&](Bytes b) {
            Bytes q = {0x00, id};
            q.insert(q.end(), b.begin(), b.end());
            out.push_back(q);
        };
        if (cmd >= 0x58 && cmd <= 0x5E && p.size() >= 4) {
            if (p[3] == drop_record) return out;
            if (p[3] == deny_record) { reply({0x80, 0x01}); return out; }
        }
        if (cmd == 0x31 && p.size() >= 4 && supported[p[3]]) {
            reply({0x86, (uint8_t)value[p[3]], (uint8_t)(value[p[3]] >> 8)});
        } else if (cmd == 0x3A) {
            auth_requests++;
            if (drop_auth) return out;
            bool ok = !reject_auth && p.size() == 11 && !memcmp(p.data() + 3, CREDENTIAL, 8);
            authorized = authorized || ok;
            reply(ok ? Bytes{0x9C} : Bytes{0x80, 0x01});
        } else if (cmd == 0x37 && p.size() >= 7) {
            if (p[3] & 1) {
                setting_attempts++;
                uint16_t v = (uint16_t)(p[5] | p[6] << 8);
                if (p[4] == deny_setting || (protected_change(p[4], v) && !authorized)) {
                    reply({0x9B}); return out;
                }
                if (p[4] != ignore_setting) value[p[4]] = v;
            }
            reply({(uint8_t)(p[3] & 1 ? 0x88 : 0x87)});
        } else if (cmd == 0x58 && p.size() >= 4) {
            memset(mem + p[3] * Requester::RECORD, 0xFF, Requester::RECORD);
            reply({0x97});
        } else if (cmd >= 0x59 && cmd <= 0x5E && p.size() >= 15) {
            // The simulator stores each logical 64-byte record. Terminal wire
            // padding is asserted separately against the observed native sequence.
            size_t off = (cmd - 0x59) * Requester::CHUNK;
            for (size_t k = 0; k < (size_t)Requester::CHUNK && off + k < (size_t)Requester::RECORD; k++) {
                mem[(size_t)p[3] * Requester::RECORD + off + k] = p[4 + k];
            }
            reply({0x97});
        } else if (cmd == 0x5F && p.size() >= 5) {
            int c = p[3], r = p[4];
            if (drop_last_read && r == records - 1) return out;
            if (r >= records) {
                reply({0x80, 0x02});
            } else if (c == 1 && blank(r)) {
                reply({0x96});
            } else {
                Bytes b = {0x96};
                size_t at = (size_t)r * Requester::RECORD + (c - 1) * Requester::CHUNK;
                for (int k = 0; k < Requester::CHUNK; k++) {
                    b.push_back(at + k < (size_t)records * Requester::RECORD ? mem[at + k] : 0xFF);
                }
                reply(b);
            }
        } else if (cmd == 0x30 && p.size() >= 4) {  // the controller's ESS and telemetry RAM reads
            uint8_t ram_reply[16];
            const size_t count = assistant_ram_reply(ram_reply, p.data(), p.size(), 4);
            reply(Bytes(ram_reply + 2, ram_reply + count));
        } else if (cmd == 0x38) {
            reply({0x99, 0, 0, 0, 0, 0, 0, 0, 0});
        } else {
            auto it = table.find(Bytes(p.begin() + 2, p.end()));
            if (it != table.end()) {
                for (const Bytes &b : it->second) reply(b);
            }
        }
        return out;
    }
};

static uint8_t nr = 0x10;
static const int64_t CYCLE = 19967;

static void hear(Requester &r, uint8_t a0, uint8_t a1, uint8_t type, const uint8_t *p, size_t n, int64_t t) {
    uint8_t g[64];
    size_t k = vebus::build_frame(a0, a1, type, nr++, p, n, g, sizeof g);
    r.heard(g, k, t);
}

// One cycle at s: the sync (none while the Multi restarts), its announcement after a reset, our request
// if one is due, the Multi's replies; the MK3's request after the cutoff when it is on the bus.
static void cycle(Requester &r, Multi &m, int64_t s, int i) {
    static const uint8_t SYNC[] = {0x55, 0xA9, 0x34, 0x12}, E4[] = {0xE4, 0x12, 0x34, 0x56},
                         MK3[] = {0x00, 0xE5, 0x30, 0x0D, 0x00};
    if (m.reset_in > 0 && --m.reset_in == 0) {  // the restart: silent for 0.24 s, unmarked, then announcing
        m.silent = 12;
        m.marked = false;
    }
    if (m.silent > 0) {
        if (--m.silent == 0) m.announce = 7;
        r.tick(s + 19000);
        return;
    }
    hear(r, 0x83, 0x83, 0xFD, SYNC, sizeof SYNC, s);
    if (m.announce == 7 && m.mk3_hello) {
        static const uint8_t HELLO[] = {0x0F, 0x06, 0x29};
        hear(r, 0x8F, 0xF7, 0xFC, HELLO, sizeof HELLO, s + 300);
    }
    uint8_t f[64], p[32] = {};
    size_t n = 0;
    int64_t earliest = 0, latest = 0;
    if (r.due(s + 700, f, sizeof f, &n, &earliest, &latest)) {
        int64_t start = earliest > s + 700 ? earliest : s + 700;
        r.sent(f, n, start, start + (int64_t)n * Requester::BYTE_US + Requester::ADDRESS_PAUSE_US);
        nr++;
        hear(r, 0x83, 0x83, 0xFE, E4, sizeof E4, s + 15030);  // counts ours
        size_t pn = vebus::payload_of(f, n, p, sizeof p);
        Bytes req(p, p + pn);
        m.sent.push_back(req);
        std::vector<Bytes> replies = m.answer(req);
        if (!replies.empty()) hear(r, 0x83, 0x83, 0xFE, replies[0].data(), replies[0].size(), s + 16080);
        if (replies.size() > 1) m.later = replies[1];
    } else if (!m.later.empty()) {
        hear(r, 0x83, 0x83, 0xFE, m.later.data(), m.later.size(), s + 16080);
        m.later.clear();
    }
    if (m.announce > 0) {
        Bytes a{0x21, 0x00, 0x00, 0x00, 0x00, 0x10, 0, 0, 0, 0, 0xFF, 0x04};
        m.put_firmware(a, 6);
        hear(r, 0x83, 0x83, 0xFE, a.data(), a.size(), s + 17500);
        if (--m.announce == 0) m.break_x = 0x30;  // and after the last, a break frame
    }
    if (m.break_x) {  // heard as is: the break takes the address and the checksum fails
        const uint8_t b[] = {0x00, 0xFB, m.break_x, (uint8_t)(0x7F - m.break_x), 0xFF};
        r.heard(b, sizeof b, s + 18200);
        m.break_x = 0;
    }
    r.tick(s + 19000);
    if (m.mk3 && i % 2 == 0) hear(r, 0x8F, 0xF7, 0xFE, MK3, sizeof MK3, s + 19300);
}

// Runs cycles from *i until the uploader is done or failed; `hook` may change the bus as it goes.
template <typename F>
static void run(Requester &r, Multi &m, upload::Uploader &u, int64_t t0, int &i, F hook, int64_t utc = 0) {
    int first = i;
    for (int end = i + 30000; i < end && u.busy(); i++) {
        int64_t s = t0 + i * CYCLE;
        hook(i);
        cycle(r, m, s, i);
        u.step(r, s + 19500, utc ? utc + (i - first) * CYCLE / 1000000 : 0);
    }
}

// A requester whose controller has listened past start-up, as the bus master or, with an MK3, paused.
static void settle(Requester &r, Multi &m, int64_t t0, int &i) {
    r.control(true);
    for (int end = i + 250; i < end; i++) cycle(r, m, t0 + i * CYCLE, i);
}


static unsigned write_requests(const Multi &m) {
    unsigned n = 0;
    for (const Bytes &p : m.sent)
        if (p.size() >= 7 && p[0] == 0 && p[2] == 0x37 && (p[3] & 1)) ++n;
    return n;
}

static unsigned reset_requests(const Multi &m) {
    unsigned n = 0;
    for (const Bytes &p : m.sent)
        if (p.size() >= 6 && p[0] == 0x21 && p[5] == 3) ++n;
    return n;
}


// Build every source byte synthetically. The eight-byte profile ID is public metadata.
static Bytes fixture(unsigned version_bytes = 8, bool blank = false, bool modern = false, unsigned records = 64) {
    static Requester::SettingsRead r;
    r = Requester::SettingsRead(); r.done = true; r.unit_ok = true; r.firmware = 2628459;
    const uint8_t version[] = {0x6B, 0x1B, 0x28, 0, 8, 0x0D, 0, 0, 0xA5, 0x5A};
    memcpy(r.version, version, version_bytes); r.version_bytes = uint8_t(version_bytes);
    r.version_ok = version_bytes == 10;
    if (modern) {
        const uint8_t v[] = {0x4B, 0x08, 0x28, 0, 8, 0x1D, 0, 0, 0x43, 0x10};
        memcpy(r.version, v, sizeof v); r.version_bytes = 10; r.version_ok = true; r.firmware = 2623563;
    }
    const uint8_t id[] = {1, 2, 3, 4}, info[] = {47, 1, 1, 1, 0, 0, 0, 0, 0xE0, 0};
    memcpy(r.unit_id, id, sizeof id); memcpy(r.unit_info, info, sizeof info);
    if (blank) r.unit_info[8] = 0xF0;
    snprintf(r.serial, sizeof r.serial, "BZSIM000000");
    memset(r.block_got, 3, sizeof r.block_got);
    for (auto &s : r.s) s.state = Requester::UNSUPPORTED;
    auto &s = r.s[2]; s.state = Requester::READ_OK; s.scale = -100;
    s.def = 5760; s.min = 4800; s.max = 6400; s.value = 5600;
    r.grid_availability = modern ? Requester::GridAvailability::AVAILABLE : Requester::GridAvailability::LEGACY_OMITTED;
    r.grid.ended = modern;
    r.assistants.ended = true; r.assistants.count = int(records);
    if (!blank) {
        r.assistants.len[0] = 64; memset(r.assistants.data[0], 0xFF, 64);
        r.assistants.data[0][0] = 0x11; r.assistants.data[0][63] = 0x22;
        r.assistants.len[63] = 64; memset(r.assistants.data[63], 0xFF, 64);
        r.assistants.data[63][63] = 0x33;
    }
    Bytes bytes(rvsc::MAX_SIZE);
    const size_t n = rvsc::build(r, uint32_t(UTC), bytes.data(), bytes.size());
    CHECK(n); bytes.resize(n); return bytes;
}

// Encode a full literal area so the test can touch both ends and record boundaries.
static Bytes image_file(Bytes base, bool remove = false, bool blank_tail = false, unsigned capacity = 4096) {
    const size_t at = settings_offset(base) + 384;
    base.erase(base.begin() + at, base.end());
    Bytes image(capacity, 0xFF);
    if (!remove) {
        image[0] = 0x91; image[63] = 0xA2; image[64] = 0xB3; image[65] = 0xD4;
        image[73] = 0xE5; image[74] = 0xF6; image[capacity-1] = 0xC4;
    }
    const unsigned literal = remove ? 0 : blank_tail ? 75 : capacity;
    if (blank_tail) image[capacity-1] = 0xFF;
    base.push_back(uint8_t(literal)); base.push_back(uint8_t(literal >> 8));
    base.insert(base.end(), image.begin(), image.begin() + literal);
    const unsigned fill = capacity - literal;
    base.push_back(0xFF); base.push_back(uint8_t(fill)); base.push_back(uint8_t(fill >> 8));
    base.insert(base.end(), 4, 0); // section checksum
    const size_t next = unit_payload(base) - 4;
    for (unsigned i = 0; i < 4; ++i) base[next + i] = uint8_t(base.size() >> (8 * i));
    base[unit_info_offset(base) + 8] = remove ? 0xF0 : 0xE0;
    fix_checksums(base); return base;
}

static void test_case(const char *name, unsigned scenario) {
    const bool add = scenario == 1, remove = scenario == 2, modern = scenario == 10;
    const unsigned records = scenario == 11 ? 63 : 64;
    const Bytes base = fixture(scenario == 8 ? 10 : 8, add, modern, records);
    Bytes edited = image_file(base, remove, scenario == 9, records*64);
    static rvsc::File original, desired;
    char error[180] = {};
    CHECK(rvsc::parse(base.data(), base.size(), original, error, sizeof error));
    CHECK(rvsc::parse(edited.data(), edited.size(), desired, error, sizeof error));
    Multi multi; multi.init(original);
    if (scenario == 12) multi.records = 0;
    if (scenario == 3) multi.deny_record = 63;
    if (scenario == 4) multi.ignore_final_mark = true;
    if (scenario == 5) multi.drop_record = 63;
    Requester requester; upload::Uploader uploader;
    const int64_t t0 = 1000000; int index = 0;
    settle(requester, multi, t0, index);
    const char *why = nullptr;
    const bool started = uploader.load(edited.data(), edited.size(), "synthetic-assistant.rvsc", error, sizeof error) &&
                         uploader.start(requester, t0 + index * CYCLE, &why);
    CHECK(started);
    bool damaged = false;
    if (started) run(requester, multi, uploader, t0, index, [&](int) {
        if (!damaged && uploader.state() == upload::VERIFYING) {
            if (scenario == 6) multi.mem[4095] ^= 1;
            if (scenario == 7) multi.drop_last_read = true;
            damaged = true;
        }
    }, UTC);
    const bool positive = scenario <= 1 || scenario == 9 || scenario == 10;
    const auto &r = requester.settings();
    bool passed = started && !uploader.busy() && (modern ? multi.grid_queries > 0 : multi.grid_queries == 0) && multi.setting_attempts == 0 &&
                  (uploader.state() == upload::DONE) == positive;
    if (positive) {
        passed = passed && multi.program_writes > 0 && multi.unit_writes == 2 && multi.resets == 1 &&
                 reset_requests(multi) == 2 && multi.identifies > 0 && requester.write_job().identified &&
                 !memcmp(multi.mem, desired.assistants, 4096) && !memcmp(multi.unit_info, desired.unit_info, 10) &&
                 r.done && r.version_bytes == (modern ? 10 : 8) && r.assistants.ended && r.assistants.count == 64 &&
                 rvsc::ready(r, &why) && !memcmp(r.unit_info, desired.unit_info, 10);
        for (int i = 0; i < 64; ++i) for (int k = 0; k < 64; ++k)
            passed = passed && (k < r.assistants.len[i] ? r.assistants.data[i][k] : 0xFF) == desired.assistants[i*64+k];
        unsigned last_chunks = 0, first_record = 999, previous_record = 999, previous_chunk = 0;
        bool tail_padding_ok = false, previous_blank_erased = false;
        for (const Bytes &frame : multi.sent) {
            if (frame.size() < 4 || frame[0] != 0 || frame[2] < 0x58 || frame[2] > 0x5E) continue;
            const unsigned record = frame[3], chunk = frame[2] - 0x58;
            if (first_record == 999) first_record = record;
            passed = passed && (record < previous_record || (record == previous_record && chunk + 1 == previous_chunk));
            previous_record = record; previous_chunk = chunk;
            if (chunk == 6 && record < 63) {
                const uint8_t pad0 = desired.assistants[(record + 1)*64 + (modern ? 0 : 9)];
                const uint8_t pad1 = desired.assistants[(record + 1)*64 + (modern ? 1 : 10)];
                passed = passed && frame.size() == 15 && frame[13] == pad0 && frame[14] == pad1;
            }
            if (record == 63) {
                ++last_chunks;
                if (chunk == 6) tail_padding_ok = frame.size() == 15 && frame[13] == (modern ? 0xFF : 0xF0) &&
                                                frame[14] == (modern ? 0xFF : 0x00);
            }
            if (record == 62 && chunk == 0) previous_blank_erased = true;
        }
        passed = passed && first_record == 63 && last_chunks == 6 && tail_padding_ok;
        if (remove || scenario == 9) passed = passed && previous_blank_erased;
        static rvsc::Check after;
        passed = passed && rvsc::check(desired, r, after, UTC + index*CYCLE/1000000) && !after.plan.assistants;
    } else {
        passed = passed && uploader.state() == upload::FAILED;
        if (scenario == 3 || scenario == 5)
            passed = passed && multi.unit_writes == 1 && multi.resets == 0 && multi.unit_info[8] == 0xF0;
        if (scenario >= 4 && scenario <= 7 && scenario != 5)
            passed = passed && multi.program_writes > 0 && multi.resets == 1;
        if (scenario == 2 || scenario == 8 || scenario >= 11)
            passed = passed && multi.unit_writes == 0 && multi.program_writes == 0 && multi.resets == 0;
    }
    CHECK(passed);
    printf("{\"case\":\"%s\",\"passed\":%s,\"expected_success\":%s,\"done\":%s,"
           "\"assistant_writes\":%d,\"unit_writes\":%d,\"reset_requests\":%u,\"resets\":%d,"
           "\"fresh_read_records\":%d,\"message\":\"%s\"}\n", name, passed ? "true" : "false",
           positive ? "true" : "false", uploader.state() == upload::DONE ? "true" : "false",
           multi.program_writes, multi.unit_writes, reset_requests(multi), multi.resets, r.assistants.count, uploader.message());
}

static void parser_failures() {
    const Bytes valid = image_file(fixture());
    static rvsc::File parsed; char error[180];
    CHECK(rvsc::parse(valid.data(), valid.size(), parsed, error, sizeof error));
    for (unsigned missing : {1u, 4u, 65u, 4096u})
        CHECK(!rvsc::parse(valid.data(), valid.size() - missing, parsed, error, sizeof error));
    Bytes too_large = valid;
    const size_t at = settings_offset(too_large) + 384;
    too_large[at] = 1; too_large[at + 1] = 16; // literal length 4097 exceeds fixed 4 KiB area
    fix_checksums(too_large);
    CHECK(!rvsc::parse(too_large.data(), too_large.size(), parsed, error, sizeof error));
    Bytes corrupt = valid; corrupt[at + 2 + 4095] ^= 1; // no checksum repair
    CHECK(!rvsc::parse(corrupt.data(), corrupt.size(), parsed, error, sizeof error));
}

int main() {
    const char *names[] = {"replace-complete-4k", "add-assistant", "remove-assistant", "record-rejected",
                           "final-mark-ignored", "record-no-ack", "last-byte-readback-mismatch",
                           "last-record-readback-timeout", "ten-byte-identity-refused", "blank-tail-native-padding", "modern-padding-unchanged",
                           "short-matching-file-and-live-bank-refused", "empty-live-bank-refused"};
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; ++i) test_case(names[i], i);
    parser_failures();
    return failures ? 1 : 0;
}
