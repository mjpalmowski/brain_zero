// The listener's own requests: when one may go out, and what became of it. Plain C++ (no Arduino),
// so test/requester_test.cpp can run it on a PC; main.cpp does the transmitting itself (quiet-bus
// check, driver enable) and reports here what it heard and sent.
//
// Two users share it:
//
// The controller, the ESP32 as the bus master (control(), target()). Once it has listened for 3 s,
// and whenever no other master (an MK3) has been heard for 10 s, it polls the Multi the way Venus
// does, so the dashboard keeps its values without an MK3: a snapshot of the powers about every 0.6 s
// (triggered with 21 00 00 00 00 0E 0E 10 0F 1A, which names RAM 14, 16, 15 and 26 and gets no reply,
// then read with ReadSnapShot 120 ms later, as Venus does; read without a trigger it comes back
// empty), the DC and L1 info frames every 1 s, and the ESS flag word with the SOC every 5 s. Given a
// target by the ESS settings (ess_policy.h, through main.cpp), it also writes the ESS setpoint every
// 2.5 s, WriteViaID RAM 131 (0x83), flags 0x00, as Venus does, at most MAX_CTL_WATTS either way; and
// the flag word, RAM 132 (0x84), whenever the Multi's three ESS bits differ from the target (its other
// bits are written back as read, as Venus does), then reads it back.
// Both wait for a check read of RAM 128 and 130 that finds the ESS assistant where Venus found it,
// repeated after every spell of another master. It falls silent the moment another master is heard
// and picks up again 10 s after the last one; three failed or five unanswered requests, a failed
// check or a refused write make it back off for 30 s.
//
// Reading every setting (read_settings()), as Venus's VE.Bus backup does (docs/vebus_config_upload_
// 2026-10-26.md): the serial number (Winmon 0x3B) and firmware (0x05), the unit's ID and model bytes
// (21 00 00 00 00 05, which the MK3 also sends at start-up), then GetSettingInfo for each setting ID and
// ReadSetting for each one the Multi supports; then the rest of what the backup file holds (rvsc.h):
// Winmon 0x3C for 00 01 3C 3D 52, answered twice each (0x9E, 0x9F), and the "grid info" (0x57) and
// assistant (0x5F) records, chunks 1 to 6 of record r, 11 bytes each, until the Multi answers 0x80; an
// assistant record whose first chunk comes back empty is blank. The arguments are mk2vsc's. Started
// from the page; it runs in the cycles the controller's polls leave free and, while another master is
// on the bus, in the cycles that master has not claimed, ending late in the slot like every request here.
//
// Writing a settings file (start_write(), planned by rvsc.h and run by upload.h): the changed settings,
// and the assistants with a reset, as Venus's mk2vsc wrote VRM's upload; only as the bus master.
//
// Test runs from the Pi (start()): reads ("tx"), the same off the bus ("bench"), or timed setpoint
// writes ("ess"); the controller waits while one runs.
//   read:      98 F7 FE <nr> 00 <id> 30 0D 00 <cs> FF       ReadRAMVar 13 (SOC), 0 (mains V)
//   write:     98 F7 FE <nr> 00 <id> 37 00 83 <lo> <hi> <cs> FF
//
// Every request goes out straight after a sync frame from the Multi, numbered one above the last
// frame heard, timed to end 2.4 ms after the sync: just before the 2.6 ms cutoff, so the Multi
// answers it in the same cycle while an MK3 request has the least chance of following it into the
// same cycle and being dropped (docs/vebus_findings_2026-10-25.md). The Multi answers one request
// per cycle, the first after the previous cycle's cutoff, so a request goes out only if no other
// master has sent anything since then; otherwise the cycle is left to it ("yielded"). Request ids
// rotate C0..C7 (the MK3 uses E8..EB, or E4..E7 from 8F F7).
//
// Each request is checked afterwards. If the transceiver hears its own frame, this echo must match
// what was sent. One whose receiver is off while it transmits hears nothing; then the next frame from
// the others must carry our frame number + 1, showing they counted ours. A request heard garbled, or
// neither heard nor counted, has failed.
//
// Sync bits 4/5 depend on the assistant program and are not interpreted as ESS state. Matched
// setpoint acknowledgements and flag readbacks are decoded separately by vebus_state.cpp.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "inverter_controls.h"
#include "charger_settings.h"
#include "ac_input.h"
#include "pv_disable.h"
#include "settings_access.h"
#include "fault_report.h"

namespace vebus {

struct InverterWriteProfile;

struct TxStats {  // the current or last test run
    uint32_t sent = 0;
    uint32_t echo_ok = 0;  // heard back intact
    uint32_t counted = 0;  // not heard back, but the next frame took the number after ours
    uint32_t failed = 0;   // heard back garbled, or neither heard nor counted
    uint32_t answered = 0, same_cycle = 0, unanswered = 0;
    uint32_t writes = 0, acked = 0;  // ESS mode: setpoint writes, and WriteRamOK replies to them
    uint32_t yielded = 0;            // cycles left to another master's request
    uint32_t busy = 0, late = 0;     // attempts given up because the bus was not quiet, or the window closed
    uint32_t bad = 0, gaps = 0;      // others' frames with a bad checksum, frame-number gaps, while switched on
    float last_reply_ms = -1;        // from the end of our request to the start of the reply
    int last_code = -1, last_values[2] = {-1, -1};
};

struct CtlStats {  // the controller, since boot
    uint32_t polls = 0, answered = 0, unanswered = 0, failed = 0;
    uint32_t writes = 0, acked = 0, flag_writes = 0, flag_acks = 0, yielded = 0, backoffs = 0;
    uint32_t late_own_echoes = 0; // exact actual-TX echoes attributed after delayed receive timestamps
};
struct SocSync {
    uint32_t soc_sent=0,current_sent=0;
    int64_t last_soc=-1,last_current=-1;
    float target_soc=0;
};

class Requester {
  public:
    void external_battery(bool valid,float soc,float extra_current_a){
        external_soc_valid_=valid;external_soc_raw_=int(soc*2+0.5f);
        extra_current_raw_=int(extra_current_a*10+0.5f);external_battery_enabled_=true;
    }
    const SocSync &soc_sync()const{return soc_sync_;}
    enum class Mode : uint8_t { OFF, READ, BENCH, ESS };
    enum class CtlState : uint8_t { OFF, TESTING, LISTENING, PAUSED, BACKING_OFF, POLLING, CHECKING, WRITING };

    static constexpr int64_t BYTE_US = 40;           // 250,000 baud, 8N1
    static constexpr int64_t ADDRESS_PAUSE_US = 8;   // 2 bit times after the address, as Victron's senders do
    static constexpr int64_t TARGET_END_US = 2400;   // end this long after the sync starts
    static constexpr int64_t WINDOW_END_US = 2500;   // and never later (cutoff 2.57-2.60 ms)
    static constexpr int64_t CLAIM_US = 2400;        // another master's frame ending later than this after a
                                                     // sync claims the next cycle's reply
    static constexpr int64_t PERIOD_US = 1000000;    // test reads: at most one a second
    static constexpr int64_t WRITE_PERIOD_US = 2500000;   // setpoint writes, as Venus
    static constexpr int64_t SNAPSHOT_PERIOD_US = 500000, INFO_PERIOD_US = 1000000, FLAGS_PERIOD_US = 5000000;
    static constexpr int64_t SNAP_READ_US = 120000;  // trigger to read; Venus waits 99-149 ms
    static constexpr int64_t ECHO_START_US = 300;    // the echo starts with our frame, give or take the timestamps' error
    static constexpr int64_t ECHO_TIMEOUT_US = 50000;     // nothing heard at all after ours: no traffic (bench)
    static constexpr int64_t REPLY_TIMEOUT_US = 200000;   // the MK3's replies come 13.5-34.4 ms after its request
    static constexpr int64_t BENCH_QUIET_US = 5000000;    // bench mode only on a bus silent this long
    static constexpr int64_t OTHER_MASTER_QUIET_US = 10000000;  // no other master for this long before we act
    // After a restart, listen this long first: an MK3 talks several times a second, and the Multi drops
    // to passthrough 13.3 s after the last setpoint, so a restart while ESS runs from here must be quick.
    static constexpr int64_t BOOT_LISTEN_US = 3000000;
    static constexpr int64_t CTL_BACKOFF_US = 30000000;
    static constexpr int64_t MAX_TX_S = 600, MAX_BENCH_S = 60;
    static constexpr int MAX_ESS_WATTS = 100;        // test runs
    static constexpr int MAX_CTL_WATTS = 5000;       // the controller; the ESS settings' safety cap is lower
    static constexpr int NOT_WRITTEN = -32768;
    static constexpr uint32_t MAX_FAILED = 3, MAX_BAD = 5, MAX_UNANSWERED_IN_A_ROW = 5, MAX_UNACKED_WRITES = 3;

    // Test runs: switch on for `seconds` (capped), or off with seconds <= 0 or Mode::OFF. Bench mode
    // sends reads without a Multi, for a transceiver test off the bus, and refuses while anyone else is
    // heard. ESS mode writes `watts` (+ = export) and is refused outside +-MAX_ESS_WATTS or while
    // another master has been heard in the last OTHER_MASTER_QUIET_US.
    void start(int64_t now, int64_t seconds, Mode mode, int watts = 0);
    void stop(const char *why);
    bool on() const { return on_; }
    Mode mode() const { return on_ ? mode_ : Mode::OFF; }

    // The controller: polling on or off (main.cpp switches it on at boot), and the ESS settings'
    // latest decision: write `watts` (+ = export, held within +-MAX_CTL_WATTS) every 2.5 s, or stop
    // writing; and keep the Multi's flag word at `flags` (-1: leave it alone).
    void control(bool enable);
    void target(bool write, int watts, int flags);
    CtlState control_state(int64_t now) const;
    const char *control_state_name(int64_t now) const;
    const char *firmware_gate(int64_t now) const;
    const char *settings_write_firmware_gate(int64_t now) const;
    const char *ess_firmware_gate(int64_t now) const;
    uint32_t inverter_firmware() const { return inverter_firmware_; }
    uint8_t ess_setpoint_ram() const { return write_profile_ ? ess_discovered_ram_ : 0; }
    uint8_t ess_discovered_ram(int64_t now) const;
    int64_t controls_invalidated_at() const { return controls_invalidated_at_; }
    bool ignore_ac_transfer_pending(int64_t now) const;
    int last_written() const { return ctl_written_w_; }  // the setpoint last sent, or NOT_WRITTEN (since another master)
    int multi_flags() const { return multi_flags_; }     // the Multi's flag word as last read, or -1
    bool settings_busy() const { return write_.active || settings_.active || operation_.active() || charger_.active() || fault_report_.active(); }
    bool read_fault_report(int64_t now, char *why, size_t cap);
    const faults::Report &fault_report() const { return fault_report_; } // cached only
    void monitor_alarm_status(bool enabled) { monitor_alarm_status_ = enabled; }
    void ac_input_target(bool ignore) { ac_input_.target(ignore); }
    const acinput::Wire &ac_input_wire() const { return ac_input_; }
    void pv_target(bool value,int64_t now,bool refresh=false){pv_.target(value,now,refresh);}
    const pvdisable::Wire &pv_wire()const{return pv_;}

    // One-shot operating controls. Never saved or replayed after another master takes over.
    bool operate(int64_t now, const inverter::Command &c, const State &st, char *why, size_t cap);
    inverter::View inverter_view(int64_t now, const State &st) const;
    void observe(const State &st, int64_t now); // decoded device readback, after each received frame
    // Stored settings require a verified model, fresh descriptors and readback per write.
    bool read_charger(int64_t now, char *why, size_t cap, charger::Profile profile = charger::Profile::CHARGER);
    void watch_charger(int64_t now); // background refresh after supported firmware is identified
    bool set_charger(int64_t now, const charger::Change &change, char *why, size_t cap, charger::Profile profile = charger::Profile::CHARGER);
    charger::View charger_view(int64_t now, charger::Profile profile = charger::Profile::CHARGER) const;
    // Allow-listed, read-only RAM/metadata probe; ESS writes retain priority.
    static constexpr int METER_COUNT = 22;
    struct MeterValue { uint8_t id=0; int16_t scale=0,offset=0; uint16_t raw=0; bool info=false,known=false; float value=0; };
    struct MeterProbe { bool active=false,done=false; int index=0; bool info=true; int64_t started=-1; MeterValue values[METER_COUNT]; };
    bool probe_meters(int64_t now);
    const MeterProbe &meter_probe() const { return meter_probe_; }

    // Reading every setting: setting IDs 0 to SETTINGS - 1, as many as Venus's backup file holds.
    static constexpr int SETTINGS = 192;
    static constexpr int BLOCK_ARGS = 5, BLOCK_BYTES = 16;  // Winmon 0x3C: 8 bytes from each reply
    static constexpr int RECORD = 64, CHUNK = 11, CHUNKS = 6;
    static constexpr int MAX_GRID_RECORDS = 48, MAX_RECORDS = 64;  // Assistant areas up to 4 KiB
    enum SettingState : uint8_t { NOT_READ, UNSUPPORTED, READ_OK, READ_FAILED };
    enum ReadPart : uint8_t {
        SERIAL_PART, VERSION_PART, UNIT_PART, SETTINGS_PART, BLOCK_PART, GRID_PART, ASSISTANTS_PART, PARTS
    };
    struct Setting {
        uint8_t state = NOT_READ;
        uint8_t access = 0;          // the info reply's last byte: 0x00, 0x80 or 0x81 on this Multi
        int16_t scale = 0, offset = 0;
        uint16_t def = 0, min = 0, max = 0, value = 0;
    };
    template <int Capacity> struct Records {  // the 0x57 or 0x5F records read so far
        uint8_t len[Capacity];       // bytes of each, 0..64 (0: blank)
        uint8_t data[Capacity][RECORD];
        int count = 0;               // records complete
        bool ended = false;          // the Multi answered 0x80 after the last
    };
    enum class GridAvailability : uint8_t {
        UNKNOWN, AVAILABLE, LEGACY_OMITTED  // omitted by a verified export format, not an unsupported command
    };
    struct SettingsRead {
        bool active = false, done = false;
        bool inspection = false;
        uint8_t part = SERIAL_PART;  // ReadPart
        int step = 0;                // the setting ID, 0x3C argument or record within the part
        uint8_t chunk = 1;           // records: 1..6
        bool info_done = false;      // the current setting's info is in; its value is next
        uint8_t tries = 0;
        int read = 0, unsupported = 0, failed = 0;
        char serial[16] = "";
        uint32_t firmware = 0;
        uint8_t version[10] = {};    // bounded storage for 0x05's actual reply data
        uint8_t version_bytes = 0;   // received data bytes; an eight-byte legacy reply is never padded
        bool version_ok = false, unit_ok = false; // full ten bytes; qualified legacy writes check actual length separately
        uint8_t unit_id[4] = {}, unit_info[10] = {};  // from 21 .. 05
        uint8_t block[BLOCK_ARGS][BLOCK_BYTES] = {};
        uint8_t block_got[BLOCK_ARGS] = {};           // bit 0 the 0x9E reply, bit 1 the 0x9F
        Records<MAX_GRID_RECORDS> grid;
        GridAvailability grid_availability = GridAvailability::UNKNOWN;
        Records<MAX_RECORDS> assistants;
        const char *problem = nullptr;  // why a part was given up
        int64_t started = -1, finished = -1;
        Setting s[SETTINGS];
    };
    // Full backups are read-only and deliberately do not require a qualified
    // write/ESS profile. Short setup inspection has its own identity checks.
    bool read_settings(int64_t now, bool inspection = false);
    bool inspection_ready(int64_t now) const;
    const SettingsRead &settings() const { return settings_; }
    static const char *part_name(int part);
    static int progress(const SettingsRead &r);  // 0..100, roughly by the requests each part takes

    // Writing a settings file (planned by rvsc::plan(), run by upload.h), as Venus's mk2vsc wrote VRM's
    // upload of 2026-10-27 (docs/vebus_config_upload_2026-10-26.md):
    //   - each setting that differs: WriteViaID, flags 01 (a setting, kept in EEPROM), answered 0x88
    //   - if the assistants differ: the unit's assistant flag set to f0 ("none") with 21 <ID> 04 and
    //     the unit's bytes 2-10; the records from the last to the first, Winmon 0x5E down to 0x59
    //     writing chunks 6 to 1 (`<record> <11 bytes>`), 0x58 `<record>` blanking a blank record whose
    //     next record is blank too, each answered 0x97; the flag set to the file's; the reset,
    //     21 00 00 00 00 03 twice
    //   - once the Multi is back (WRITE_RESTART_US), the start-up an MK3 goes through after a reset:
    //     21 FF FF FF FF 80, a second later 83, two seconds later the roll call (21 .. 01 00 twice,
    //     21 .. 00, which the unit answers, 21 <ID> 01 01, then 21 .. 00 twice, a second apart)
    // Only as the bus master: start_write() refuses while another master is heard, and the job stops
    // if one starts talking before the assistant flag has been set; after that it goes on in the cycles
    // the other master leaves free, as the yield rule allows, and leaves the start-up to it. No polls,
    // setpoint or flag-word writes go out while the job runs; afterwards the ESS check is made again.
    static constexpr int64_t WRITE_RESTART_US = 4000000;  // reset to start-up; the MK3 took 5.8 s
    enum WriteStep : uint8_t {
        W_AUTHORIZE, W_SETTINGS, W_MARK, W_RECORDS, W_UNMARK, W_RESET, W_RESTART, W_STARTUP, W_DONE
    };
    struct WritePlan {
        SettingsAccess access;                    // RAM only, erased after the job
        int64_t read_started = -1;                // the full live snapshot this plan was checked against
        bool access_only = false;                 // credential diagnostic: cannot write or restart
        uint8_t required_access = 0;
        bool restart = false;                     // settings or assistants require reset/startup
        int settings = 0;                         // to write, in this order
        uint8_t setting_id[SETTINGS] = {};
        uint16_t setting_value[SETTINGS] = {};
        bool assistants = false;                  // write the records and reset
        bool disable_assistants = false;          // legacy: write F0 and restart, without programming records
        bool retain_assistants = false;           // verify image[] stays unchanged, including scalar-only writes
        uint8_t unit_id[4] = {};
        uint8_t unit_marked[9] = {}, unit_final[9] = {};  // 21 <ID> 04's bytes: flag f0, then the file's
        int records = 0;                          // assistant records (44 on this Multi)
        uint8_t image[MAX_RECORDS * RECORD + 2];  // the assistant memory, blank bytes 0xFF; +2: the last chunk
        uint8_t blank[MAX_RECORDS] = {};          // 1: no data in the record
    };
    struct WriteJob {
        bool active = false, done = false, failed = false;
        uint8_t step = W_SETTINGS;   // WriteStep
        int index = 0;               // the setting, record or start-up frame
        uint8_t chunk = 6;           // records: 6..1, or 0 for 0x58
        uint8_t tries = 0;
        int settings_written = 0, records_written = 0;
        bool identified = false;     // the roll call's 21 .. 00 was answered by the unit
        bool startup_skipped = false;  // another master was on the bus after the reset
        bool maintenance = false;     // standalone restart/redetect, without any EEPROM writes
        unsigned authorized = 0;
        unsigned access_requests = 0, config_sends = 0, reset_sends = 0;
        int access_reply = -1;
        int64_t authorization_deadline = -1;
        char startup[96] = "";       // what became of the start-up after the reset, for the messages
        char problem[112] = "";      // why it failed
        int64_t started = -1, finished = -1, wait_until = -1, reset_at = -1;
    };
    // Starts the plan's writes, or refuses with *why: another master on the bus, the controller off,
    // a test run, a settings read or another write job running, or nothing to write.
    bool start_write(int64_t now, const WritePlan &plan, const char **why, int64_t utc = 0);
    bool start_access_test(int64_t now, const SettingsAccess &access, const char **why, int64_t utc);
    const WriteJob &write_job() const { return write_; }
    const WritePlan &write_plan() const { return plan_; }
    static const char *write_step_name(int step);
    static int write_progress(const WriteJob &j, const WritePlan &p);  // 0..100

    // Every frame the listener hears, in order. Returns true if it was our own echo. *counted is set
    // when this frame shows that the others counted our last frame, which the listener did not hear;
    // sent_frame() then gives that frame, for the decoder.
    bool heard(const uint8_t *raw, size_t n, int64_t t0, bool *counted = nullptr);
    const uint8_t *sent_frame(size_t *n, int64_t *t_start) const;
    // True if a request may go out: `frame` then holds it. It should start at *earliest_start and must
    // start by *latest_start.
    bool due(int64_t now, uint8_t *frame, size_t cap, size_t *n, int64_t *earliest_start, int64_t *latest_start);
    void sent(const uint8_t *frame, size_t n, int64_t t_start, int64_t t_end);
    void gave_up(bool late);  // the attempt found the bus busy (or the window closed)
    void tick(int64_t now);   // timeouts and the controller's state; call often

    // Lines for the log ("# tx", "# ess", "# control"); take_report() hands them over and clears them.
    bool take_report(char *out, size_t cap);
    size_t describe(int64_t now, char *out, size_t cap) const;  // for the status line
    size_t to_json(int64_t now, char *out, size_t cap) const;   // "\"tx\":{...},\"ctl\":{...}"
    const TxStats &stats() const { return stats_; }
    const CtlStats &ctl_stats() const { return ctl_; }

  private:
    enum Phase : uint8_t { IDLE, ECHO, REPLY };
    enum Kind : uint8_t {
        READ, CHECK, WRITE, SNAPSHOT, DC_INFO, L1_INFO, FLAGS, SNAP_TAKE, FLAGS_WRITE,
        CHARGE_STATUS, AC_VERSION, AC_WRITE, AC_STATUS, OPERATE, PV_WRITE,
        CHARGER_VERSION, CHARGER_UNIT, CHARGER_INFO, CHARGER_READ, CHARGER_WRITE,
        SOC_SEND, EXTRA_CURRENT_SEND,
        SERIAL_READ, VERSION_READ, INFO_READ, SETTING_READ,      // reading every setting, from here on
        UNIT_READ, BLOCK_READ, GRID_READ, ASSISTANT_READ,
        METER_INFO, METER_READ, FAULT_READ,
        ACCESS_WRITE, SETTING_WRITE, UNIT_WRITE, RECORD_WRITE, RESET_SEND, STARTUP_SEND,  // the write job
        ALARM_CAPABILITY, ALARM_STATUS, ESS_UNIT, ESS_REGISTERS,
        KINDS
    };
    void note(const char *fmt, ...);
    bool test_job(int64_t now, Kind *kind);
    bool control_job(int64_t now, Kind *kind);
    bool alarm_poll() const { return by_control_ && (kind_ == ALARM_CAPABILITY || kind_ == ALARM_STATUS); }
    void finish(int64_t t_reply, const uint8_t *p, size_t k);
    void finish_control(int64_t t_reply, const uint8_t *p, size_t k, const char *head);
    void failed_one();
    void other_heard(const uint8_t *raw, size_t n, int64_t t0, bool intact);
    void report_control(int64_t now);
    void operation_end(bool ok, const char *message);
    bool charger_ready(int64_t now, char *why, size_t cap) const;
    const char *charger_gate(int64_t now, charger::Profile profile) const;
    bool charger_job(Kind *kind) const;
    void charger_reply(const uint8_t *p, size_t k);
    void charger_end(bool ok, const char *message);
    void charger_begin(int64_t now, const charger::Change *change, charger::Profile profile);
    void invalidate_charger(const char *message, bool retry_soon = true);
    void invalidate_inverter_configuration(const char *message, bool break_frame = false);
    void invalidate_runtime_controls(const char *message, bool preserve_ignore_ac = false);
    void clear_pending_echo();
    void observe_firmware(const uint8_t *p, size_t k);
    bool runtime_firmware_verified() const;
    void reset_ess_layout();
    bool ess_discovery_available(int64_t now) const;
    Kind ess_layout_job() const;
    const char *ess_layout_reply(const uint8_t *p, size_t k);
    void meter_reply(const uint8_t *p, size_t k);
    void back_off(const char *why);
    bool settings_job(Kind *kind);
    void settings_reply(const uint8_t *p, size_t k);
    template <int Capacity> void records_reply(Records<Capacity> &rec, uint8_t code, const uint8_t *p, size_t k);
    void settings_next();
    void settings_part_done();
    void settings_give_up(const char *why);
    void settings_finish(const char *problem);
    bool inspection_link_ready(int64_t now) const;
    bool write_job(int64_t now, Kind *kind);
    void write_reply(const uint8_t *p, size_t k);
    void write_advance();
    void write_end(const char *problem);  // done, or failed with `problem`
    bool write_marked() const { return write_.active && !write_.maintenance &&
        ((plan_.assistants && write_.step > W_MARK) || (plan_.disable_assistants && write_.step >= W_RESET)); }
    size_t write_payload(Kind kind, uint8_t id, uint8_t *payload) const;

    // test runs
    bool on_ = false;
    Mode mode_ = Mode::OFF;
    int watts_ = 0;
    int64_t until_ = 0;
    char why_[80] = "";
    const char *stop_why_ = nullptr;  // set by a reply that ends the run, acted on once the request is done
    TxStats stats_;
    uint32_t unanswered_in_a_row_ = 0, unacked_in_a_row_ = 0;
    int64_t next_due_ = 0;
    uint8_t next_id_ = 0;  // request ids, shared with the controller
    uint8_t ess_discovered_ram_ = 0;
    uint8_t ess_header_ = 0, ess_candidate_ = 0;
    bool ess_chain_complete_ = false, ess_absent_ = false;
    int64_t ess_retry_at_ = 0;
    int64_t last_write_ = -1;  // ours, test run or controller, for the stopping message

    // the controller
    bool ctl_enabled_ = false, ctl_armed_ = false;
    const InverterWriteProfile *write_profile_ = nullptr;
    bool ess_discovery_supported_ = false;
    uint32_t inverter_firmware_ = 0;
    uint32_t control_generation_ = 0, sent_control_generation_ = 0;
    int64_t next_firmware_probe_ = 0, controls_invalidated_at_ = -1, ignore_ac_transfer_until_ = -1;
    bool snap_taken_ = false;  // a snapshot was triggered (and counted) and waits to be read
    int ctl_watts_ = 0, ctl_written_w_ = NOT_WRITTEN;
    int ctl_flags_ = -1, multi_flags_ = -1;  // the flag word wanted, and the Multi's as last read
    int64_t ctl_next_[KINDS] = {};
    bool monitor_alarm_status_ = false;
    int64_t ctl_backoff_until_ = -1, ctl_last_write_ = -1;
    uint32_t ctl_failed_in_a_row_ = 0, ctl_unanswered_in_a_row_ = 0, ctl_unacked_in_a_row_ = 0;
    CtlState ctl_reported_ = CtlState::OFF;
    CtlStats ctl_;
    bool external_battery_enabled_=false,external_soc_valid_=false,extra_current_pending_=false;
    bool external_current_active_=false; // a nonzero broadcast needs a zero release when ESS stops
    int external_soc_raw_=0,extra_current_raw_=0;
    SocSync soc_sync_;
    acinput::Wire ac_input_;
    pvdisable::Wire pv_;
    inverter::Job operation_;
    charger::Status charger_;
    charger::Status charger_saved_[2]; // independent readbacks; one serialized bus job
    charger::Change charger_change_;
    uint8_t charger_index_ = 0, charger_order_[charger::COUNT] = {}, charger_cursor_ = 0, charger_tries_ = 0;
    bool charger_info_ = true, charger_version_pending_ = true, charger_unit_pending_ = false;
    bool charger_write_info_checked_ = false, charger_unit_known_ = false;
    uint8_t charger_model_ = 0, charger_voltage_ = 0, charger_unit_[14] = {};
    int64_t charger_next_read_ = 0;
    uint32_t charger_generation_ = 0, planned_charger_generation_ = 0, sent_charger_generation_ = 0;
    MeterProbe meter_probe_;
    uint32_t meter_generation_=0,planned_meter_generation_=0,sent_meter_generation_=0;
    faults::Report fault_report_;
    uint32_t planned_fault_attempt_=0,sent_fault_attempt_=0;
    SettingsRead settings_;
    uint32_t settings_generation_ = 0, planned_settings_generation_ = 0, sent_settings_generation_ = 0;
    WritePlan plan_;
    WriteJob write_;

    // the bus as heard
    int last_nr_ = -1;
    int64_t sync_t0_ = -1, prev_sync_t0_ = -1;  // starts of the Multi's last two sync frames
    bool clean_ = false;                        // no bad frame since the last sync
    int64_t first_heard_ = -1, last_other_ = -1;  // first frame heard; last valid frame not our own
    int64_t other_end_ = -1;                    // end of the last frame from another master
    uint8_t other_addr_[2] = {};                // its first two bytes: the address, if it was intact
    bool other_intact_ = false;
    int64_t yielded_sync_ = -1;                 // the sync whose cycle was last left to another master
    int64_t now_ = 0;                           // the latest time seen, for decisions made in finish()

    // the request in flight
    Phase phase_ = IDLE;
    Kind kind_ = READ, planned_kind_ = READ;
    bool by_control_ = false, planned_by_control_ = false;
    int value_ = 0, planned_value_ = 0;  // what a write carries: the setpoint or the flag word
    uint8_t frame_[48];  // fits a 15-byte payload (a record chunk), escaped at worst
    size_t frame_len_ = 0;
    // A single actual transmission, independent of any frame merely prepared by
    // due(). May contain access credentials: volatile-wiped on every clear.
    uint8_t pending_echo_[48] = {};
    uint8_t pending_echo_len_ = 0;
    bool late_echo_decoded_ = false; // ownership-only backfill suppression for the first following frame
    int64_t pending_echo_start_ = -1, pending_echo_deadline_ = -1;
    int64_t t_start_ = 0, t_end_ = 0, at_sync_ = -1;
    bool expect_reply_ = true;  // not in bench mode
    uint8_t syncs_since_ = 0;   // syncs heard since it went out: 0 = a reply in the same cycle
    const char *echo_ = "";     // ok, BAD (garbled), counted (not heard, but counted) or lost

    char notes_[1024];
    size_t notes_len_ = 0;
};

}  // namespace vebus
