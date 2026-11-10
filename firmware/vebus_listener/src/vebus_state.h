// What the listener knows about the Multi, decoded from the frames on the bus. Plain C++ (no
// Arduino), so test/replay.cpp can run it on a PC against recorded frames.
//
// Frame layouts: our own measurements (docs/vebus_findings_2026-10-25.md) for the snapshot reply,
// the ESS setpoint and flag word, and the 38 04 SOC frame; GitNik1/VEBus (GPL-3.0) and
// burlizzi/victron (MIT) for the info, MasterMultiLED (0x41), 0x80, 0x70 and E4 frames. Each was
// checked against Venus's readings on this MultiPlus-II 48/5000 on 2026-10-26.
// The monitoring fields for additional phases and inputs follow Victron's MK2 protocol §6.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace vebus {

constexpr int64_t NEVER = -1;

struct Reading {
    float v = 0;
    int64_t t = NEVER;  // listener time of the frame it came from, µs
};

struct AcPhase {
    float mains_v = 0, mains_a = 0, inverter_v = 0, inverter_a = 0;
    int64_t t = NEVER;
    int device_state = -1;
    bool mains_a_known = false, inverter_a_known = false;
};

struct AcMonitor {
    AcPhase phases[4];            // L1 through L4 in wire phase order
    int phase_count = -1;         // number advertised by the L1 info frame
    int input_count = -1;         // MasterMultiLED, revision 415 or higher
    int last_input = -1;          // one-based last used input, independent of relay state
    int64_t input_t = NEVER;
};

struct State {
    // bus
    uint32_t frames = 0, bad = 0, gaps = 0, multi_frames = 0, master_frames = 0, own_frames = 0;
    int64_t last_multi = NEVER, last_master = NEVER;
    float sync_ms = 0;            // smoothed sync period
    uint32_t inverter_firmware = 0;
    int64_t inverter_firmware_at = NEVER;  // matched full Winmon reply, current bus session only
    // snapshot reply (0x99), W
    Reading inverter_w;           // + = charging the battery
    Reading acout_w;              // + = drawn at AC-out; - = AC-coupled solar exceeds the loads
    Reading acin_w;               // + = export to the grid
    // battery
    Reading bat_v;                // E4 frame, every cycle
    Reading bat_a;                // 0x80 frame, + = charging
    Reading charge_a, discharge_a;  // DC info frame
    Reading dc_info_v;             // DC info frame voltage, separate from the E4 reading
    Reading soc;                  // %: 0x70 frame, the master's 38 04 frame or its RAM 13 read
    // AC side, L1 info frame (frequency from E4)
    Reading mains_v, mains_a, inverter_v, inverter_a, ac_hz;
    AcMonitor ac_monitor;          // monitor only, independent of the single-phase control fields
    int device_state = -1;
    int64_t device_state_t = NEVER;
    // MasterMultiLED (0x41)
    int led_on = -1, led_blink = -1;
    int64_t led_t = NEVER;
    Reading limit_a, limit_min_a, limit_max_a;
    int ac_input_config = -1, switch_register = -1, actual_mode = -1;
    int64_t actual_mode_t = NEVER;
    int charge_main_state = -1, charge_state = -1;
    int64_t charge_state_t = NEVER;
    int temp_raw = -1;            // 0x80 byte 11 when present; its scale is unconfirmed
    // ESS observations. Sync bits 4/5 depend on the assistant program; they do not reliably say
    // whether ESS is running (see the 2026-10-27 configuration-upload findings).
    Reading sync_status;          // raw byte, for diagnostics only
    Reading setpoint_w;           // last WriteViaID to the ESS setpoint (Venus's or ours), + = export
    Reading setpoint_readback_w;  // a matched RAM read of the ESS setpoint, independent of write ACKs
    Reading accepted_setpoint_w;  // last matched WriteRamOK's value and the original WRITE time
    int64_t setpoint_ack_t = NEVER;  // when that acknowledgement was heard
    uint32_t setpoint_writes = 0, setpoint_acks = 0;
    Reading flags;                // confirmed ESS flag word: RAM read or acknowledged write

    // Recent acceptance is evidence about the setpoint, not proof of the Multi's operating state.
    // Time it from the write, conservatively inside the shortest measured 13.3 s watchdog timeout.
    int setpoint_fresh(int64_t now) const {
        if (!bus_fresh(now) || accepted_setpoint_w.t == NEVER || now < accepted_setpoint_w.t) return -1;
        return now - accepted_setpoint_w.t < 13000000 ? 1 : 0;
    }
    int discharge_disabled(int64_t now) const {
        if (!bus_fresh(now) || flags.t == NEVER || now < flags.t || now - flags.t >= 30000000) return -1;
        return ((int)flags.v & 2) != 0;
    }
    bool bus_fresh(int64_t now) const {
        return last_multi != NEVER && now >= last_multi && now - last_multi < 5000000;
    }
    // the Multi's own controls (0x3F), sent only on change
    int mode_cmd = -1;            // 4 Off, 5 charger only, 6 inverter only, 7 On; last command, not readback
    int64_t mode_t = NEVER;
    Reading limit_cmd_a;
};

// Checksum test, payload extraction and frame building, as in tools/vebus_timing/vebus.py.
bool checksum_ok(const uint8_t *raw, size_t n);
// The short frame the Multi sends after a UART break, as heard: 00 FB xx yy FF, xx + yy = 0x7F (the
// break reads as 00 and takes the address). Heard when a master switches the assistants off and after
// the Multi's restart, in VRM's upload and in the ESP32's (2026-10-27); it fails the checksum.
bool multi_break_frame(const uint8_t *raw, size_t n);
size_t payload_of(const uint8_t *raw, size_t n, uint8_t *out, size_t cap);
// Escape 0xFA..0xFF in the payload, add the checksum (with its 0xFA pad when needed) and 0xFF.
size_t build_frame(uint8_t addr0, uint8_t addr1, uint8_t type, uint8_t nr, const uint8_t *payload, size_t n,
                   uint8_t *out, size_t cap);

class Decoder {
  public:
    // One complete frame, ending in 0xFF. `own` marks the listener's own request (requester.h): it is
    // decoded like Venus's, but not counted as Venus talking.
    void feed(const uint8_t *raw, size_t n, int64_t t, bool own = false);
    // The live controller supplies its checked ESS register or zero when unverified.
    bool set_ess_register(uint8_t setpoint);
    const State &state() const { return s_; }

  private:
    void clear_ess_observations();
    void reset_monitor();
    void monitor_ac(const uint8_t *p, int64_t t);
    void multi(const uint8_t *p, size_t n, int64_t t);
    void master(const uint8_t *p, size_t n, int64_t t);
    State s_;
    int last_nr_ = -1;
    int64_t last_sync_ = NEVER;
    int dc_inverter_factor_ = 0;
    int64_t dc_factor_t_ = NEVER;
    uint8_t ess_setpoint_ram_ = 131;  // known layout for standalone replay of the original captures
    struct Request {
        uint8_t cmd = 0, a0 = 0, a1 = 0;  // Winmon command and its first two argument bytes
        int16_t value = 0;                // WriteViaID's value, for a matched acknowledgement
        int64_t sent_at = NEVER;          // consumed on a RAM read/write reply; invalidated on reset
    } req_[256];                          // by request id: replies carry it back
};

// The state as one JSON object. Readings become [value, age in s], or null if never seen.
// `extra` (may be null) is appended as further members, e.g. "\"rssi\":-60".
size_t to_json(const State &s, int64_t now, const char *extra, char *out, size_t cap);

}  // namespace vebus
