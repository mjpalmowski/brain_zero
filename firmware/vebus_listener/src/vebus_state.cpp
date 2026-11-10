#include "vebus_state.h"

#include "multiplus_ii.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace vebus {

namespace {

constexpr uint8_t EOF_BYTE = 0xFF, SYNC = 0xFD, DATA = 0xFE, SYNC_MARK = 0x55;
constexpr uint8_t RAM_SOC = 13;

unsigned u16(const uint8_t *p, size_t i) { return p[i] | p[i + 1] << 8; }
int s16(const uint8_t *p, size_t i) { return (int16_t)u16(p, i); }
unsigned long u24(const uint8_t *p, size_t i) { return p[i] | (unsigned long)p[i + 1] << 8 | (unsigned long)p[i + 2] << 16; }

void set(Reading &r, float v, int64_t t) {
    r.v = v;
    r.t = t;
}

}  // namespace

bool checksum_ok(const uint8_t *raw, size_t n) {
    if (n < 6 || raw[n - 1] != EOF_BYTE) return false;
    unsigned total = 0;
    for (size_t i = 2; i < n; i++) total += raw[i];
    if (raw[2] == SYNC && raw[4] == SYNC_MARK) total -= SYNC_MARK;
    return (total & 0xFF) == 0;
}

bool multi_break_frame(const uint8_t *raw, size_t n) {
    return n == 5 && raw[0] == 0x00 && raw[1] == 0xFB && ((raw[2] + raw[3]) & 0xFF) == 0x7F && raw[4] == EOF_BYTE;
}

size_t payload_of(const uint8_t *raw, size_t n, uint8_t *out, size_t cap) {
    if (n < 6) return 0;
    size_t end = n - 2;                     // drop the checksum and 0xFF
    if (end > 4 && raw[end - 1] == 0xFA) end--;  // 0xFA put before a checksum of 0xFB..0xFF
    size_t k = 0;
    for (size_t i = 4; i < end && k < cap; i++) {
        if (raw[i] == 0xFA && i + 1 < end && raw[i + 1] >= 0x7A && raw[i + 1] <= 0x7F) {
            out[k++] = raw[++i] | 0x80;
        } else {
            out[k++] = raw[i];
        }
    }
    return k;
}

size_t build_frame(uint8_t addr0, uint8_t addr1, uint8_t type, uint8_t nr, const uint8_t *payload, size_t n,
                   uint8_t *out, size_t cap) {
    if (cap < 6 + 2 * n) return 0;
    size_t k = 0;
    out[k++] = addr0;
    out[k++] = addr1;
    out[k++] = type;
    out[k++] = nr & 0x7F;
    for (size_t i = 0; i < n; i++) {
        if (payload[i] >= 0xFA) {
            out[k++] = 0xFA;
            out[k++] = 0x70 | (payload[i] & 0x0F);
        } else {
            out[k++] = payload[i];
        }
    }
    unsigned total = 0;
    for (size_t i = 2; i < k; i++) total += out[i];
    if (type == SYNC) total -= SYNC_MARK;
    uint8_t cs = (uint8_t)(1 - total);
    if (cs >= 0xFB) {
        out[k++] = 0xFA;
        cs = (uint8_t)(cs - 0xFA);
    }
    out[k++] = cs;
    out[k++] = EOF_BYTE;
    return k;
}

void Decoder::feed(const uint8_t *raw, size_t n, int64_t t, bool own) {
    s_.frames++;
    if (!checksum_ok(raw, n)) {
        s_.bad++;
        if (multi_break_frame(raw, n)) {  // assistants stopped, or the Multi reset
            clear_ess_observations();
            s_.actual_mode_t = s_.charge_state_t = s_.led_t = s_.limit_a.t = NEVER;
            reset_monitor();
            for (Request &r : req_) r.sent_at = NEVER;
        }
        return;
    }
    int nr = raw[3] & 0x7F;
    if (last_nr_ >= 0 && ((nr - last_nr_) & 0x7F) != 1) s_.gaps++;
    last_nr_ = nr;
    bool from_multi = raw[0] == 0x83 && raw[1] == 0x83;
    if (from_multi) {
        if (s_.last_multi != NEVER && !s_.bus_fresh(t)) reset_monitor();
        s_.multi_frames++;
        s_.last_multi = t;
    } else if (own) {
        s_.own_frames++;
    } else {
        s_.master_frames++;
        s_.last_master = t;
    }
    if (raw[2] == SYNC) {
        if (from_multi && last_sync_ != NEVER) {
            float ms = (t - last_sync_) / 1000.0f;
            if (ms > 15 && ms < 25) s_.sync_ms = s_.sync_ms > 0 ? s_.sync_ms + (ms - s_.sync_ms) / 50 : ms;
        }
        if (from_multi) {
            last_sync_ = t;
            uint8_t p[8];
            if (payload_of(raw, n, p, sizeof p) >= 2) {
                set(s_.sync_status, p[1], t);
            }
        }
        return;
    }
    if (raw[2] != DATA) return;
    uint8_t p[300];
    size_t k = payload_of(raw, n, p, sizeof p);
    if (k == 0) return;
    if (from_multi) {
        multi(p, k, t);
    } else {
        master(p, k, t);
    }
}

void Decoder::clear_ess_observations() {
    s_.setpoint_w.t = s_.setpoint_readback_w.t = s_.accepted_setpoint_w.t = NEVER;
    s_.setpoint_ack_t = s_.flags.t = NEVER;
}

bool Decoder::set_ess_register(uint8_t setpoint) {
    if (setpoint < 129 || setpoint == 255) setpoint = 0;
    if (setpoint == ess_setpoint_ram_) return false;
    const int previous = ess_setpoint_ram_;
    ess_setpoint_ram_ = setpoint;
    clear_ess_observations();
    for (Request &r : req_) {
        // Preserve the fresh read that established a new layout. Old layout reads and pending
        // writes cannot establish observations on the new layout.
        if (r.cmd == 0x37 || (r.cmd == 0x30 && previous &&
            (r.a0 == previous || r.a0 == previous + 1 || r.a1 == previous || r.a1 == previous + 1)))
            r.sent_at = NEVER;
    }
    return true;
}

void Decoder::reset_monitor() {
    s_.ac_monitor = AcMonitor();
    s_.dc_info_v.t = dc_factor_t_ = NEVER;
    s_.inverter_firmware = 0;
    s_.inverter_firmware_at = NEVER;
}

void Decoder::monitor_ac(const uint8_t *p, int64_t t) {
    AcMonitor &monitor = s_.ac_monitor;
    const int phase = p[5] >= 0x08 ? 0 : 0x08 - p[5];
    if (phase == 0) {
        monitor.phase_count = p[5] - 0x07;
        for (int i = monitor.phase_count; i < 4; ++i) monitor.phases[i] = AcPhase();
    } else if (monitor.phase_count >= 0 && phase >= monitor.phase_count &&
               t - monitor.phases[0].t < 5000000) {
        return;
    }

    AcPhase &sample = monitor.phases[phase];
    sample.t = t;
    sample.mains_v = u16(p, 6) / 100.0f;
    sample.inverter_v = u16(p, 10) / 100.0f;
    sample.mains_a_known = p[1] != 0;
    sample.mains_a = s16(p, 8) / 100.0f * p[1];
    sample.inverter_a_known = dc_inverter_factor_ != 0 && dc_factor_t_ != NEVER &&
                              t >= dc_factor_t_ && t - dc_factor_t_ < 5000000;
    sample.inverter_a = s16(p, 12) / 100.0f * dc_inverter_factor_;
    sample.device_state = s_.inverter_firmware % 1000 >= 207 && p[4] <= 9 ? p[4] : -1;
}

void Decoder::multi(const uint8_t *p, size_t n, int64_t t) {
    switch (p[0]) {
    case 0x00: {  // reply: 00 <request id> <reply code> ...
        if (n < 3) break;
        Request &r = req_[p[1]];
        if (p[2] == 0x99 && n >= 9) {  // ReadSnapShot: inverter, AC-out, AC-in power
            set(s_.inverter_w, s16(p, 3), t);
            set(s_.acout_w, s16(p, 5), t);
            set(s_.acin_w, s16(p, 7), t);
        } else if (p[2] == 0x82 && n >= 7 && r.cmd == 0x05 && r.sent_at != NEVER &&
                   t >= r.sent_at && t - r.sent_at <= 200000) {  // matched SoftwareVersionPart0
            const uint32_t version = u16(p, 3) | (uint32_t)u16(p, 5) << 16;
            const uint32_t firmware = version >= 1000000 && version <= 9999999 ? version : 0;
            if (s_.inverter_firmware && s_.inverter_firmware != firmware) reset_monitor();
            s_.inverter_firmware = firmware;
            // The documented long reply has four version bytes followed by two reserved bytes.
            // Newer replies can carry more data. Those extra bytes are only needed by write gates.
            s_.inverter_firmware_at = firmware && n >= 9 ? t : NEVER;
            r.sent_at = NEVER;
        } else if (p[2] == 0x85 && r.cmd == 0x30 && r.sent_at != NEVER &&
                   t >= r.sent_at && t - r.sent_at <= 200000) {  // matched RamReadOK
            for (int j = 0; j < 2; j++) {
                uint8_t var = j ? r.a1 : r.a0;
                size_t at = 3 + 2 * j;
                if (at + 1 >= n) break;
                if (var == RAM_SOC) set(s_.soc, u16(p, at) * 0.5f, t);
                if (ess_setpoint_ram_ && var == ess_setpoint_ram_ + 1) set(s_.flags, u16(p, at), t);
                if (ess_setpoint_ram_ && var == ess_setpoint_ram_) set(s_.setpoint_readback_w, s16(p, at), t);
            }
            r.sent_at = NEVER;
        } else if (p[2] == 0x94 && n >= 5 && r.cmd == 0x0E && r.sent_at != NEVER &&
                   t >= r.sent_at && t - r.sent_at <= 200000) {
            s_.charge_main_state = p[3];
            s_.charge_state = p[4];
            s_.charge_state_t = t;
            r.sent_at = NEVER;
        } else if (r.cmd == 0x37) {  // match RAM write ACKs to an observed, still pending request
            if (ess_setpoint_ram_ && p[2] == 0x87 && !(r.a0 & 1) && r.sent_at != NEVER &&
                t >= r.sent_at && t - r.sent_at <= 200000) {
                if (r.a1 == ess_setpoint_ram_) {
                    s_.setpoint_acks++;
                    if (r.sent_at >= s_.accepted_setpoint_w.t) {
                        set(s_.accepted_setpoint_w, r.value, r.sent_at);
                        s_.setpoint_ack_t = t;
                    }
                } else if (r.a1 == ess_setpoint_ram_ + 1 && r.sent_at >= s_.flags.t) {
                    set(s_.flags, (uint16_t)r.value, t);
                }
            }
            r.sent_at = NEVER;
        }
        break;
    }
    case 0x20:  // info frame; byte 5 says which
        if (n < 14) break;
        if (p[5] == 0x0C) {  // DC
            set(s_.dc_info_v, u16(p, 6) / 100.0f, t);
            dc_inverter_factor_ = p[2];
            dc_factor_t_ = t;
            set(s_.discharge_a, u24(p, 8) / 10.0f, t);
            set(s_.charge_a, u24(p, 11) / 10.0f, t);
        } else if (p[5] >= 0x05 && p[5] <= 0x0B) {
            monitor_ac(p, t);
            if (p[5] == 0x08) {  // retain the existing L1 values used by the 563 control path
                set(s_.mains_v, u16(p, 6) / 100.0f, t);
                set(s_.mains_a, s16(p, 8) / 100.0f * p[1], t);
                set(s_.inverter_v, u16(p, 10) / 100.0f, t);
                set(s_.inverter_a, s16(p, 12) / 100.0f * p[2], t);
                s_.device_state = p[4];
                s_.device_state_t = t;
            }
        }
        break;
    case 0x41: {  // MasterMultiLED: on, blink, status, AC input config, min/max/actual limit, switches
        if (n < 12) break;
        s_.led_on = p[2];
        s_.led_blink = p[3];
        s_.led_t = t;
        s_.ac_input_config = p[5];
        AcMonitor &monitor = s_.ac_monitor;
        const bool count_known = !(p[5] & 8) && s_.inverter_firmware % 1000 >= 415;
        monitor.input_count = count_known ? (p[5] >> 4) & 7 : -1;
        const int input = (p[5] & 3) + 1;
        monitor.last_input = input <= monitor.input_count ? input : -1;
        monitor.input_t = count_known ? t : NEVER;
        if (!(p[5] & 8)) { // dedicated DMC: these limit bytes must be ignored
            set(s_.limit_min_a, u16(p, 6) / 10.0f, t);
            set(s_.limit_max_a, u16(p, 8) / 10.0f, t);
            set(s_.limit_a, u16(p, 10) / 10.0f, t);
        } else {
            s_.limit_min_a.t = s_.limit_max_a.t = s_.limit_a.t = NEVER;
        }
        if (n >= 13) {
            s_.switch_register = p[12];
            s_.actual_mode = 4 + ((p[12] >> 4) & 3);
            s_.actual_mode_t = t;
        }
        break;
    }
    case 0x80:  // charger/inverter condition: battery current, sometimes a temperature
        if (n < 7 || p[1] != 0x80 || (p[2] & 0xFE) != 0x12 || p[4] != 0x80) break;
        set(s_.bat_a, s16(p, 5) / 10.0f, t);
        if (n > 11 && (p[7] & 0xF0) == 0x30) s_.temp_raw = p[11];
        break;
    case 0x70:  // battery frame; on this Multi bytes 6-7 carry the SOC in 0.5 % (layout varies)
        if (n >= 8 && p[4] == 0xBC && p[5] == 0x02) set(s_.soc, u16(p, 6) * 0.5f, t);
        break;
    case 0xE4:  // every cycle: AC frequency in mHz, DC voltage in 1/50 V (low 12 bits)
        if (n < 14) break;
        set(s_.ac_hz, u16(p, 2) / 1000.0f, t);
        set(s_.bat_v, (((p[13] & 0x0F) << 8) | p[12]) / 50.0f, t);
        break;
    default:
        break;
    }
}

void Decoder::master(const uint8_t *p, size_t n, int64_t t) {
    switch (p[0]) {
    case 0x00: {  // request: 00 <id> <Winmon command> <args>
        if (n < 3) break;
        Request &r = req_[p[1]];
        r.cmd = p[2];
        r.a0 = n > 3 ? p[3] : 0;
        r.a1 = n > 4 ? p[4] : 0;
        r.sent_at = t;
        if (p[2] == 0x37 && n >= 7 && !(p[3] & 0x01)) {  // WriteViaID to a RAM variable
            r.value = s16(p, 5);
            if (ess_setpoint_ram_ && p[4] == ess_setpoint_ram_) {
                set(s_.setpoint_w, s16(p, 5), t);
                s_.setpoint_writes++;
            }
        }
        break;
    }
    case 0x38:  // External SOC/current broadcasts are commands, not inverter readback.
        break;
    case 0x3F:  // the Multi's own controls
        if (n >= 5 && p[1] == 0xC0) {
            set(s_.limit_cmd_a, u16(p, 3) / 10.0f, t);
        } else if (n >= 2) {
            s_.mode_cmd = p[1];
            s_.mode_t = t;
        }
        break;
    default:
        break;
    }
}

namespace {

struct Json {
    char *out;
    size_t cap, len = 0;
    bool first = true;
    Json(char *o, size_t c) : out(o), cap(c) {}
    void raw(const char *fmt, ...) {
        if (len >= cap) return;
        va_list a;
        va_start(a, fmt);
        int n = vsnprintf(out + len, cap - len, fmt, a);
        va_end(a);
        if (n > 0) len += (size_t)n < cap - len ? (size_t)n : cap - len - 1;
    }
    void key(const char *k) {
        raw(first ? "\"%s\":" : ",\"%s\":", k);
        first = false;
    }
    void num(const char *k, double v, int decimals) {
        key(k);
        raw("%.*f", decimals, v);
    }
    void count(const char *k, unsigned long v) {
        key(k);
        raw("%lu", v);
    }
    void age(const char *k, int64_t t, int64_t now) {
        key(k);
        if (t == NEVER) raw("null"); else raw("%.1f", (now - t) / 1e6);
    }
    void reading(const char *k, const Reading &r, int64_t now, int decimals) {
        key(k);
        if (r.t == NEVER) raw("null"); else raw("[%.*f,%.1f]", decimals, (double)r.v, (now - r.t) / 1e6);
    }
    void integer(const char *k, int v) {  // -1 = never seen
        key(k);
        if (v < 0) raw("null"); else raw("%d", v);
    }
};

void ac_monitor_json(Json &j, const AcMonitor &m, int64_t now) {
    j.key("ac_monitor");
    j.raw("{");
    j.first = true;
    j.integer("phase_count", m.phase_count);
    j.age("phase_count_age_s", m.phases[0].t, now);
    j.integer("input_count", m.input_count);
    j.integer("last_input", m.last_input);
    j.age("input_age_s", m.input_t, now);
    j.key("phases");
    j.raw("[");
    for (int i = 0; i < 4; ++i) {
        if (i) j.raw(",");
        const AcPhase &phase = m.phases[i];
        if (phase.t == NEVER) {
            j.raw("null");
            continue;
        }
        j.raw("{");
        j.first = true;
        j.age("age_s", phase.t, now);
        j.num("mains_v", phase.mains_v, 2);
        if (phase.mains_a_known) j.num("mains_a", phase.mains_a, 2);
        else j.integer("mains_a", -1);
        j.num("inverter_v", phase.inverter_v, 2);
        if (phase.inverter_a_known) j.num("inverter_a", phase.inverter_a, 2);
        else j.integer("inverter_a", -1);
        j.integer("device_state", phase.device_state);
        j.raw("}");
    }
    j.raw("]}");
    j.first = false;
}

}  // namespace

size_t to_json(const State &s, int64_t now, const char *extra, char *out, size_t cap) {
    if (cap == 0) return 0;
    out[0] = 0;
    Json j(out, cap);
    j.raw("{");
    j.num("now_s", now / 1e6, 1);
    j.count("frames", s.frames);
    j.count("bad", s.bad);
    j.count("gaps", s.gaps);
    j.count("multi_frames", s.multi_frames);
    j.count("master_frames", s.master_frames);
    j.count("own_frames", s.own_frames);
    j.age("multi_age_s", s.last_multi, now);
    j.age("master_age_s", s.last_master, now);
    j.num("sync_ms", s.sync_ms, 3);
    j.reading("inverter_w", s.inverter_w, now, 0);
    j.reading("acout_w", s.acout_w, now, 0);
    j.reading("acin_w", s.acin_w, now, 0);
    j.reading("bat_v", s.bat_v, now, 2);
    j.reading("bat_a", s.bat_a, now, 1);
    j.reading("charge_a", s.charge_a, now, 1);
    j.reading("discharge_a", s.discharge_a, now, 1);
    j.reading("dc_info_v", s.dc_info_v, now, 2);
    j.reading("soc", s.soc, now, 1);
    j.reading("mains_v", s.mains_v, now, 2);
    j.reading("mains_a", s.mains_a, now, 2);
    j.reading("inverter_v", s.inverter_v, now, 2);
    j.reading("inverter_a", s.inverter_a, now, 2);
    j.reading("ac_hz", s.ac_hz, now, 3);
    j.integer("device_state", s.device_state);
    j.age("device_state_age_s", s.device_state_t, now);
    j.integer("led_on", s.led_on);
    j.integer("led_blink", s.led_blink);
    j.age("led_age_s", s.led_t, now);
    j.reading("limit_a", s.limit_a, now, 1);
    j.reading("limit_max_a", s.limit_max_a, now, 1);
    j.reading("limit_min_a", s.limit_min_a, now, 1);
    j.integer("ac_input_config", s.ac_input_config);
    j.integer("switch_register", s.switch_register);
    j.integer("actual_mode", s.actual_mode);
    j.age("actual_mode_age_s", s.actual_mode_t, now);
    j.integer("charge_state", s.charge_state);
    j.age("charge_state_age_s", s.charge_state_t, now);
    j.integer("temp_raw", s.temp_raw);
    j.integer("ess_live", -1);  // legacy field: operating state is unconfirmed, never inferred from sync bits
    j.integer("ess_no_feed_in", s.discharge_disabled(now));
    j.integer("setpoint_fresh", s.setpoint_fresh(now));
    j.reading("accepted_setpoint_w", s.accepted_setpoint_w, now, 0);
    j.age("setpoint_ack_age_s", s.setpoint_ack_t, now);
    j.reading("sync_status", s.sync_status, now, 0);
    j.reading("setpoint_w", s.setpoint_w, now, 0);
    j.count("setpoint_writes", s.setpoint_writes);
    j.count("setpoint_acks", s.setpoint_acks);
    j.reading("flags", s.flags, now, 0);
    j.integer("mode_cmd", s.mode_cmd);
    j.age("mode_age_s", s.mode_t, now);
    j.reading("limit_cmd_a", s.limit_cmd_a, now, 1);
    ac_monitor_json(j, s.ac_monitor, now);
    const bool identity_known = s.bus_fresh(now) && s.inverter_firmware_at != NEVER && now >= s.inverter_firmware_at;
    j.integer("inverter_firmware", identity_known ? int(s.inverter_firmware) : -1);
    const auto *model = identity_known ? multiplus_ii::find(s.inverter_firmware) : nullptr;
    j.key("multiplus_ii");
    if (model) {
        j.raw("{\"name\":\"%s\",\"product_id\":%u,\"firmware\":%lu,\"nominal_voltage\":%u,\"age_s\":%.1f}",
              model->name, unsigned(model->product_id), (unsigned long)s.inverter_firmware,
              unsigned(model->nominal_voltage), (now - s.inverter_firmware_at) / 1e6);
    } else {
        j.raw("null");
    }
    if (extra && *extra) j.raw(",%s", extra);
    j.raw("}");
    return j.len;
}

}  // namespace vebus
