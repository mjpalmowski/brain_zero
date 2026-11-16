#include "battery_bms.h"

#include <cmath>
#include <cstring>

// JK wire offsets and the read-request format are based on
// syssi/esphome-jk-bms, revision6b5a25aa43fb044c2a9592a5e87f06f8edc42554,
// Copyright syssi and contributors, Apache-2.0:
// https://github.com/syssi/esphome-jk-bms/blob/6b5a25aa43fb044c2a9592a5e87f06f8edc42554/LICENSE
// This bounded decoder has no dependency on the ESPHome runtime and never
// implements that component's settings, calibration, MOSFET or password writes.
namespace battery_bms {
namespace {
constexpr uint8_t JK_HEADER[] = {0x55, 0xaa, 0xeb, 0x90};
constexpr uint32_t BASIC = SOC | VOLTAGE | CURRENT;
constexpr uint32_t PACE_FIELDS[] = {
    LIMITS, SOC | SOH, VOLTAGE | CURRENT | PACK_TEMPERATURE,
    ALARMS, PERMISSIONS, IDENTITY, CELL_EXTREMA | TEMPERATURES,
};

void wipe(void *memory, size_t length) {
    volatile uint8_t *p = static_cast<volatile uint8_t *>(memory);
    while (length--) *p++ = 0;
}
uint16_t u16(const uint8_t *p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }
uint32_t u32(const uint8_t *p) { return uint32_t(u16(p)) | (uint32_t(u16(p + 2)) << 16); }
int32_t s16(const uint8_t *p) { const uint16_t v = u16(p); return v < 0x8000u ? v : int32_t(v) - 0x10000; }
int64_t delta32(uint32_t value, uint32_t previous) {
    const uint32_t d = value - previous;
    return d <= 0x7fffffffu ? int64_t(d) : int64_t(d) - 0x100000000LL;
}
int64_t signed32(const uint8_t *p) { return delta32(u32(p), 0); }
bool range(float value, float low, float high) { return std::isfinite(value) && value >= low && value <= high; }
bool fresh(int64_t at, int64_t now, int64_t ttl = FRESH_US) {
    return at >= 0 && now >= at && now - at < ttl;
}
float age(int64_t at, int64_t now) { return at >= 0 && now >= at ? float(now - at) / 1000000.f : -1.f; }
float minimum(float a, float b) { return a < b ? a : b; }
float maximum(float a, float b) { return a > b ? a : b; }
bool version_char(uint8_t c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') || c == '.' || c == '_' || c == '-';
}
bool text(char *out, size_t cap, const uint8_t *in, size_t length, bool model) {
    std::memset(out, 0, cap);
    size_t n = 0;
    while (n < length && in[n]) {
        if (n + 1 >= cap || (!version_char(in[n]) && !(model && in[n] == ' '))) return false;
        out[n] = char(in[n]);
        ++n;
    }
    if (!n) return false;
    if (model) return n >= 3 && out[0] == 'J' && out[1] == 'K';
    return out[0] >= '0' && out[0] <= '9';
}
unsigned major_version(const char *s) {
    unsigned major = 0;
    while (*s >= '0' && *s <= '9') {
        major = major * 10 + unsigned(*s++ - '0');
        if (major > 99) return 0;
    }
    return major;
}
int pace_index(uint16_t id) {
    switch (id) {
    case 0x351: return 0;
    case 0x355: return 1;
    case 0x356: return 2;
    case 0x359: return 3;
    case 0x35c: return 4;
    case 0x35e: return 5;
    case 0x373: return 6;
    default: return -1;
    }
}
}

void JkFeed::clear_assembly() {
    wipe(frame_, sizeof frame_);
    used_ = 0;
    fragment_at_ = -1;
}
void JkFeed::clear_samples() {
    sample_ = Snapshot{};
    status_at_ = -1;
    profile_ready_ = counter_seen_ = false;
    last_counter_ = 0;
}
void JkFeed::connected(bool value) {
    if (value == connected_) return;
    clear_assembly();
    clear_samples();
    connected_ = value;
}
void JkFeed::reset() {
    clear_assembly();
    clear_samples();
    stats_ = Stats{};
    connected_ = false;
}
bool JkFeed::request(uint8_t opcode, uint8_t out[20]) {
    if (!out) return false;
    wipe(out, 20);
    if (opcode != 0x96 && opcode != 0x97) return false;
    out[0] = 0xaa; out[1] = 0x55; out[2] = 0x90; out[3] = 0xeb; out[4] = opcode;
    for (size_t i = 0; i < 19; ++i) out[19] = uint8_t(out[19] + out[i]);
    return true;
}
bool JkFeed::accept(const uint8_t *data, size_t length, int64_t now) {
    if (!connected_ || !data || !length || length > 400 || now < 0) {
        ++stats_.refused;
        clear_assembly();
        return false;
    }
    if (used_ && (now < fragment_at_ || now - fragment_at_ > 2000000)) clear_assembly();
    // As on the installed bridge, a new notification preamble supersedes an
    // incomplete record. A split preamble itself is also supported below.
    if (used_ >= sizeof JK_HEADER && length >= sizeof JK_HEADER &&
        !std::memcmp(data, JK_HEADER, sizeof JK_HEADER)) clear_assembly();
    bool accepted = false;
    for (size_t i = 0; i < length; ++i) {
        const uint8_t byte = data[i];
        if (used_ < sizeof JK_HEADER) {
            if (byte != JK_HEADER[used_]) {
                clear_assembly();
                if (byte == JK_HEADER[0]) { frame_[used_++] = byte; fragment_at_ = now; }
                continue;
            }
        }
        frame_[used_++] = byte;
        fragment_at_ = now;
        if (used_ != sizeof frame_) continue;
        uint8_t checksum = 0;
        for (size_t k = 0; k < sizeof frame_ - 1; ++k) checksum = uint8_t(checksum + frame_[k]);
        if (checksum != frame_[299]) {
            ++stats_.checksum_errors;
            ++stats_.refused;
        } else if (decode(now)) {
            ++stats_.accepted;
            accepted = true;
        } else {
            ++stats_.refused;
        }
        // Identity payloads contain secrets beyond the selected public fields.
        // Erase every consumed record, including unsupported settings records.
        clear_assembly();
        // The qualified JK profile sends one record per notification stream,
        // sometimes followed by an opaque20-byte trailer. Discard that tail;
        // retaining a coincidental preamble in it could cache private bytes.
        return accepted;
    }
    return accepted;
}
bool JkFeed::decode(int64_t now) {
    if (frame_[4] == 3) return decode_identity();
    if (frame_[4] == 2) return decode_status(now);
    return false;
}
bool JkFeed::decode_identity() {
    char model[17] = {}, hardware[9] = {}, firmware[9] = {};
    const bool valid = text(model, sizeof model, frame_ + 6, 16, true) &&
                       text(hardware, sizeof hardware, frame_ + 22, 8, false) &&
                       text(firmware, sizeof firmware, frame_ + 30, 8, false);
    if (!valid) {
        clear_samples();
        return false;
    }
    if (std::strcmp(model, sample_.model) || std::strcmp(hardware, sample_.hardware) ||
        std::strcmp(firmware, sample_.firmware)) clear_samples();
    std::memcpy(sample_.model, model, sizeof model);
    std::memcpy(sample_.hardware, hardware, sizeof hardware);
    std::memcpy(sample_.firmware, firmware, sizeof firmware);
    std::memcpy(sample_.protocol, "JK02_24S", 9);
    sample_.valid |= IDENTITY;
    const unsigned major = major_version(hardware);
    profile_ready_ = major >= 6 && major < 11;
    if (!profile_ready_) { sample_.valid = IDENTITY; status_at_ = -1; counter_seen_ = false; }
    return true;
}
bool JkFeed::decode_status(int64_t now) {
    if (!profile_ready_) return false;
    const uint8_t advance = uint8_t(frame_[5] - last_counter_);
    if (counter_seen_ && (advance == 0 || advance >= 128)) { ++stats_.duplicates; return false; }
    if (status_at_ >= 0 && now <= status_at_) return false;
    Snapshot next = sample_;
    next.valid &= IDENTITY;
    next.cell_count = 0;
    next.min_cell_v = 100;
    next.max_cell_v = 0;
    const uint32_t enabled = u32(frame_ + 54);
    // This qualified series-pack profile populates consecutive cells1..N.
    // Reject holes instead of publishing a compact cell_count with missing or
    // misnumbered cell values; other layouts need their own qualification.
    bool cells_ok = enabled && !(enabled & 0xff000000u) && !(enabled & (enabled + 1u));
    float sum = 0;
    for (size_t i = 0; i < MAX_CELLS; ++i) {
        const float value = float(u16(frame_ + 6 + 2 * i)) * 0.001f;
        next.cell_v[i] = value;
        if (!(enabled & (1u << i))) {
            if (value != 0) cells_ok = false;
            continue;
        }
        if (!range(value, 0.5f, 5.f)) cells_ok = false;
        ++next.cell_count;
        sum += value;
        if (value < next.min_cell_v) { next.min_cell_v = value; next.min_cell_index = uint8_t(i + 1); }
        if (value > next.max_cell_v) { next.max_cell_v = value; next.max_cell_index = uint8_t(i + 1); }
    }
    next.voltage = float(u32(frame_ + 118)) * 0.001f;
    if (!cells_ok || next.cell_count < 4 || !range(next.voltage, 3.f, 160.f) ||
        std::fabs(sum - next.voltage) > maximum(0.5f, next.voltage * 0.02f)) {
        // A checksum-valid but structurally incompatible layout must not leave
        // previous control-quality telemetry marked as current.
        sample_.valid &= IDENTITY;
        status_at_ = -1;
        return false;
    }
    next.delta_cell_v = next.max_cell_v - next.min_cell_v;
    next.valid |= VOLTAGE | CELL_EXTREMA | CELL_VALUES;
    next.soc = frame_[141];
    if (range(next.soc, 0, 100)) next.valid |= SOC;
    next.current = float(signed32(frame_ + 126)) * 0.001f;
    if (range(next.current, -2000, 2000)) next.valid |= CURRENT;
    next.capacity_ah = float(u32(frame_ + 146)) * 0.001f;
    if (range(next.capacity_ah, 0.1f, 10000)) next.valid |= CAPACITY;
    next.remaining_ah = float(u32(frame_ + 142)) * 0.001f;
    if (next.has(CAPACITY) && range(next.remaining_ah, 0, next.capacity_ah * 1.05f)) next.valid |= REMAINING;
    next.soh = frame_[158];
    if (range(next.soh, 0, 100)) next.valid |= SOH;
    const float t1 = float(s16(frame_ + 130)) * 0.1f, t2 = float(s16(frame_ + 132)) * 0.1f;
    if (range(t1, -100, 150) && range(t2, -100, 150)) {
        next.temp_min_c = minimum(t1, t2); next.temp_max_c = maximum(t1, t2);
        next.temperature_c = (t1 + t2) * 0.5f;
        next.valid |= TEMPERATURES | PACK_TEMPERATURE;
    }
    next.temp_mos_c = float(s16(frame_ + 134)) * 0.1f;
    if (range(next.temp_mos_c, -100, 150)) next.valid |= MOS_TEMPERATURE;
    next.alarms = u16(frame_ + 136); next.warnings = 0;
    next.valid |= ALARMS;
    if (frame_[166] <= 1 && frame_[167] <= 1) {
        next.charge_allowed = frame_[166] != 0; next.discharge_allowed = frame_[167] != 0;
        next.valid |= PERMISSIONS;
    }
    next.balancing_current_a = float(s16(frame_ + 138)) * 0.001f;
    if (frame_[140] <= 2 && range(next.balancing_current_a, -20, 20)) {
        next.balancing = frame_[140] != 0;
        next.valid |= BALANCING;
    }
    next.cycles = u32(frame_ + 150); next.valid |= CYCLES;
    next.module_count = 1;
    sample_ = next;
    status_at_ = now;
    last_counter_ = frame_[5]; counter_seen_ = true;
    return true;
}
Snapshot JkFeed::view(int64_t now) const {
    Snapshot out = sample_;
    out.connected = connected_;
    if (!connected_) out.valid = 0;
    else if (!fresh(status_at_, now)) out.valid &= IDENTITY;
    out.complete = out.has(BASIC);
    out.sample_age_s = out.soc_age_s = out.cell_age_s = out.permissions_age_s = out.alarms_age_s = age(status_at_, now);
    return out;
}

void PaceFeed::clear_samples() {
    sample_ = Snapshot{};
    std::memcpy(sample_.model, "PACE CAN", 9);
    for (auto &t : at_) t = -1;
    anchor_begin_ = anchor_end_ = -1;
    anchor_ms_ = boot_id_ = last_sequence_ = 0;
    have_anchor_ = sequence_seen_ = false;
}
void PaceFeed::connected(bool value) {
    if (value == connected_) return;
    clear_samples();
    connected_ = value;
}
void PaceFeed::reset() { clear_samples(); stats_ = Stats{}; connected_ = false; }
bool PaceFeed::anchor(const uint8_t *data, size_t length, int64_t begin, int64_t end) {
    if (!connected_ || !data || length != CAN_BLE_BYTES || std::memcmp(data, "BZCB", 4) ||
        data[4] != 1 || data[5] != 1 || begin < 0 || end < begin || end - begin > 250000) {
        ++stats_.refused;
        clear_samples();
        return false;
    }
    const uint32_t boot = u32(data + 6), tick = u32(data + 10);
    if (have_anchor_ && boot == boot_id_) {
        const int64_t dt = delta32(tick, anchor_ms_) * 1000;
        const int64_t local = end - anchor_begin_;
        const int64_t minimum_local = begin - anchor_end_;
        const int64_t skew = (local >= 0 ? local : -local) / 2000 + 2000;
        // Same-boot source clocks cannot run backwards or race far ahead of
        // the timed metadata read. 500ppm plus2ms covers timer quantisation.
        if (begin < anchor_end_ || dt < 0 || dt < minimum_local - skew || dt > local + skew) {
            ++stats_.refused;
            clear_samples();
            return false;
        }
    } else if (have_anchor_) {
        clear_samples(); // different boot: every old frame and field is invalid
    }
    have_anchor_ = true; boot_id_ = boot; anchor_ms_ = tick;
    anchor_begin_ = begin; anchor_end_ = end;
    return true;
}
bool PaceFeed::accept(const uint8_t *data, size_t length, int64_t now) {
    if (!connected_ || !have_anchor_ || !data || length != CAN_BLE_BYTES || data[0] != 1 ||
        data[1] > 8 || !fresh(anchor_end_, now, ANCHOR_FRESH_US)) { ++stats_.refused; return false; }
    const uint8_t dlc = data[1];
    const uint16_t id = u16(data + 2);
    const int index = pace_index(id);
    if (index < 0) { ++stats_.refused; return false; }
    for (size_t i = dlc; i < 8; ++i) if (data[12 + i]) { ++stats_.refused; return false; }
    const uint32_t sequence = u32(data + 4);
    if (sequence_seen_ && delta32(sequence, last_sequence_) <= 0) {
        ++stats_.duplicates; ++stats_.refused; return false;
    }
    const int64_t dt = delta32(u32(data + 8), anchor_ms_) * 1000;
    if (dt < -FRESH_US || dt > ANCHOR_FRESH_US + SOURCE_TIMESTAMP_UNCERTAINTY_US) {
        ++stats_.refused; return false;
    }
    const int64_t absolute_dt = dt >= 0 ? dt : -dt;
    const int64_t drift = absolute_dt / 2000 + 2000;
    const int64_t unadjusted = anchor_begin_ + dt;
    if (unadjusted > now + SOURCE_TIMESTAMP_UNCERTAINTY_US + (anchor_end_ - anchor_begin_) + drift) {
        ++stats_.refused; return false;
    }
    // Oldest possible local reception time, including source callback delay.
    // Delayed notifications, cache replay and Wi-Fi/OTA stalls retain that age.
    const int64_t at = unadjusted - SOURCE_TIMESTAMP_UNCERTAINTY_US - drift;
    if (!fresh(at, now) || (at_[index] >= 0 && at < at_[index])) { ++stats_.refused; return false; }
    if (!decode_can(id, data + 12, dlc, at)) { ++stats_.refused; return false; }
    last_sequence_ = sequence; sequence_seen_ = true;
    ++stats_.accepted;
    return true;
}
bool PaceFeed::decode_can(uint16_t id, const uint8_t *p, uint8_t length, int64_t at) {
    const int index = pace_index(id);
    const uint32_t group = PACE_FIELDS[index];
    const uint8_t minimum_lengths[] = {6, 4, 6, 5, 1, 8, 8};
    sample_.valid &= ~group;
    at_[index] = -1;
    if (length < minimum_lengths[index]) return false;
    switch (id) {
    case 0x351:
        sample_.charge_voltage_limit_v = float(u16(p)) * 0.1f;
        sample_.charge_current_limit_a = float(s16(p + 2)) * 0.1f;
        sample_.discharge_current_limit_a = float(s16(p + 4)) * 0.1f;
        if (!range(sample_.charge_voltage_limit_v, 3, 160) ||
            !range(sample_.charge_current_limit_a, 0, 2000) ||
            !range(sample_.discharge_current_limit_a, 0, 2000)) return false;
        sample_.valid |= LIMITS;
        break;
    case 0x355:
        sample_.soc = u16(p); sample_.soh = u16(p + 2);
        if (!range(sample_.soc, 0, 100)) return false;
        sample_.valid |= SOC;
        if (range(sample_.soh, 0, 100)) sample_.valid |= SOH;
        break;
    case 0x356:
        sample_.voltage = float(u16(p)) * 0.01f;
        sample_.current = float(s16(p + 2)) * 0.1f;
        sample_.temperature_c = float(s16(p + 4)) * 0.1f;
        if (!range(sample_.voltage, 3, 160) || !range(sample_.current, -2000, 2000)) return false;
        sample_.valid |= VOLTAGE | CURRENT;
        if (range(sample_.temperature_c, -100, 150)) sample_.valid |= PACK_TEMPERATURE;
        break;
    case 0x359:
        if (!p[4] || p[4] > 16) return false;
        sample_.alarms = u16(p); sample_.warnings = u16(p + 2);
        sample_.module_count = p[4]; sample_.valid |= ALARMS;
        break;
    case 0x35c:
        if (p[0] & 7u) return false; // reserved permission bits: unknown dialect
        sample_.charge_allowed = (p[0] & 0x80) != 0;
        sample_.discharge_allowed = (p[0] & 0x40) != 0;
        sample_.force_charge = (p[0] & 0x30) != 0;
        sample_.full_charge_requested = (p[0] & 8u) != 0;
        sample_.valid |= PERMISSIONS;
        break;
    case 0x35e:
        for (size_t i = 0; i < 8; ++i) {
            if (p[i] < 0x20 || p[i] > 0x7e) return false;
        }
        std::memcpy(sample_.protocol, p, 8); sample_.protocol[8] = 0;
        sample_.valid |= IDENTITY;
        break;
    case 0x373:
        sample_.min_cell_v = float(u16(p)) * 0.001f;
        sample_.max_cell_v = float(u16(p + 2)) * 0.001f;
        if (!range(sample_.min_cell_v, 0.5f, 5) || !range(sample_.max_cell_v, sample_.min_cell_v, 5)) return false;
        sample_.delta_cell_v = sample_.max_cell_v - sample_.min_cell_v;
        sample_.valid |= CELL_EXTREMA;
        sample_.temp_min_c = float(u16(p + 4)) - 273.15f;
        sample_.temp_max_c = float(u16(p + 6)) - 273.15f;
        if (range(sample_.temp_min_c, -100, 150) && range(sample_.temp_max_c, sample_.temp_min_c, 150))
            sample_.valid |= TEMPERATURES;
        break;
    default: return false;
    }
    at_[index] = at;
    return true;
}
Snapshot PaceFeed::view(int64_t now) const {
    Snapshot out = sample_;
    out.connected = connected_;
    if (!connected_ || !have_anchor_ || !fresh(anchor_end_, now, ANCHOR_FRESH_US)) out.valid = 0;
    else for (size_t i = 0; i < 7; ++i) if (!fresh(at_[i], now)) out.valid &= ~PACE_FIELDS[i];
    out.complete = out.has(BASIC);
    out.sample_age_s = age(at_[2], now);
    out.soc_age_s = age(at_[1], now); out.cell_age_s = age(at_[6], now);
    out.limits_age_s = age(at_[0], now); out.permissions_age_s = age(at_[4], now); out.alarms_age_s = age(at_[3], now);
    return out;
}

Snapshot aggregate(const Snapshot &jk, const Snapshot &pace, float configured_pace_capacity_ah) {
    Snapshot out;
    std::memcpy(out.model, "Parallel bank", 14);
    out.connected = jk.connected && pace.connected;
    if (!out.connected) return out;
    const bool voltage_agrees = jk.has(VOLTAGE) && pace.has(VOLTAGE) && range(jk.voltage, 3, 160) &&
        range(pace.voltage, 3, 160) && std::fabs(jk.voltage - pace.voltage) <= maximum(0.5f, maximum(jk.voltage, pace.voltage) * .02f);
    if (voltage_agrees) {
        // A shared DC bus has one voltage; use the mean of its two fresh readings.
        out.voltage = (jk.voltage + pace.voltage) * .5f; out.valid |= VOLTAGE;
        if (jk.has(CURRENT) && pace.has(CURRENT) && range(jk.current, -2000, 2000) && range(pace.current, -2000, 2000)) {
            out.current = jk.current + pace.current; out.valid |= CURRENT;
        }
        const float pace_capacity = pace.has(CAPACITY) ? pace.capacity_ah : configured_pace_capacity_ah;
        if (jk.has(CAPACITY) && range(jk.capacity_ah, .1f, 10000) && range(pace_capacity, .1f, 10000)) {
            out.capacity_ah = jk.capacity_ah + pace_capacity; out.valid |= CAPACITY;
            if (jk.has(SOC) && pace.has(SOC) && range(jk.soc, 0, 100) && range(pace.soc, 0, 100)) {
                out.soc = (jk.soc * jk.capacity_ah + pace.soc * pace_capacity) / out.capacity_ah;
                out.valid |= SOC;
            }
        }
        if (jk.has(CELL_EXTREMA) && pace.has(CELL_EXTREMA) &&
            range(jk.min_cell_v, .5f, 5) && range(jk.max_cell_v, jk.min_cell_v, 5) &&
            range(pace.min_cell_v, .5f, 5) && range(pace.max_cell_v, pace.min_cell_v, 5)) {
            out.min_cell_v = minimum(jk.min_cell_v, pace.min_cell_v);
            out.max_cell_v = maximum(jk.max_cell_v, pace.max_cell_v);
            out.delta_cell_v = out.max_cell_v - out.min_cell_v; out.valid |= CELL_EXTREMA;
        }
    }
    if (jk.has(TEMPERATURES) && pace.has(TEMPERATURES)) {
        out.temp_min_c = minimum(jk.temp_min_c, pace.temp_min_c); out.temp_max_c = maximum(jk.temp_max_c, pace.temp_max_c);
        out.valid |= TEMPERATURES;
    }
    if (jk.has(PERMISSIONS) && pace.has(PERMISSIONS)) {
        out.charge_allowed = jk.charge_allowed && pace.charge_allowed;
        out.discharge_allowed = jk.discharge_allowed && pace.discharge_allowed;
        out.force_charge = jk.force_charge || pace.force_charge;
        out.full_charge_requested = jk.full_charge_requested || pace.full_charge_requested;
        out.valid |= PERMISSIONS;
    }
    if (jk.has(ALARMS) && pace.has(ALARMS)) {
        out.alarms = jk.alarms | pace.alarms; out.warnings = jk.warnings | pace.warnings; out.valid |= ALARMS;
    }
    out.sample_age_s = maximum(jk.sample_age_s, pace.sample_age_s);
    out.soc_age_s = maximum(jk.soc_age_s, pace.soc_age_s); out.cell_age_s = maximum(jk.cell_age_s, pace.cell_age_s);
    out.permissions_age_s = maximum(jk.permissions_age_s, pace.permissions_age_s);
    out.alarms_age_s = maximum(jk.alarms_age_s, pace.alarms_age_s);
    out.complete = out.has(BASIC);
    // CVL/CCL/DCL are deliberately not summed: safe sharing between dissimilar
    // parallel packs needs its own qualified current/charge-management policy.
    return out;
}
} // namespace battery_bms
