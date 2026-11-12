#include "requester.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include <initializer_list>

#include "multiplus_ii.h"
#include "vebus_state.h"

namespace vebus {

struct InverterWriteProfile {
    const uint8_t (&version)[10];
    uint8_t model;
    bool rvsc, other_runtime;
};

namespace {

constexpr int64_t NONE = -1;
constexpr uint8_t FIRST_ID = 0xC0, IDS = 8;
constexpr uint8_t RAM_MAINS_V = 0, RAM_SOC = 13;
constexpr uint8_t ASSISTANTS = 128, ESS_ASSISTANT_ID = 5;
// Winmon 0x05 reply from the validated standalone 2623563 installation.
constexpr uint8_t TESTED_VERSION[] = {0x4B, 0x08, 0x28, 0x00, 0x08, 0x1D, 0x00, 0x00, 0x43, 0x10};
// All listed profiles permit ESS only after the live standalone and Assistant checks.
constexpr uint8_t VERSION_2629556[] = {0xB4, 0x1F, 0x28, 0x00, 0x08, 0x1D, 0x00, 0x00, 0x40, 0x10};
constexpr uint8_t VERSION_2623556[] = {0x44, 0x08, 0x28, 0x00, 0x08, 0x1D, 0x00, 0x00, 0x40, 0x10};
// 24/3000 identity from a checksum-valid owner export, ESS checked in the isolated Venus replay.
constexpr uint8_t VERSION_2611552[] = {0x60, 0xD9, 0x27, 0x00, 0x08, 0x1D, 0x00, 0x00, 0x3F, 0x10};
// 48/4k5 identity and isolated ESS evidence from the same owner-export workflow.
constexpr uint8_t VERSION_2601560[] = {0x58, 0xB2, 0x27, 0x00, 0x08, 0x1D, 0x00, 0x00, 0x42, 0x10};
constexpr InverterWriteProfile WRITE_PROFILES[] = {
    {TESTED_VERSION, 0x2F, true, true},    // 48/5000, 2623563
    {VERSION_2629556, 0x2F, true, false},  // 48/3000
    {VERSION_2623556, 0x2F, false, false}, // 48/5000, ESS only
    {VERSION_2611552, 0x2F, true, false},  // 24/3000
    {VERSION_2601560, 0x2F, true, false},  // 48/4k5
};
const InverterWriteProfile *match_write_profile(const uint8_t *version, size_t len) {
    if (len < sizeof TESTED_VERSION) return nullptr;
    for (const auto &profile : WRITE_PROFILES) {
        // The original 2623563 profile also accepts a longer reply with this prefix.
        if ((len == sizeof TESTED_VERSION || profile.other_runtime) &&
            !memcmp(version, profile.version, sizeof TESTED_VERSION)) return &profile;
    }
    return nullptr;
}
constexpr uint8_t CLASSIC_12V_VERSION[] = {0x66, 0xD1, 0x27, 0x00, 0x08, 0x01, 0x00, 0x00, 0x00, 0x00};
constexpr uint8_t CLASSIC_24V_VERSION[] = {0xDC, 0xE4, 0x27, 0x00, 0x08, 0x01, 0x00, 0x00, 0x00, 0x00};
struct ChargerProfile {
    const uint8_t *version;
    uint16_t product_id;
    uint8_t model, voltage;
};
// Full Winmon replies and model bytes from the genuine VEConfigure profiles.
// These allow the three stored charger settings. Runtime controls still require TESTED_VERSION.
constexpr ChargerProfile CHARGER_PROFILES[] = {
    {CLASSIC_12V_VERSION, 0x2609, 0x02, 12},
    {CLASSIC_24V_VERSION, 0x2614, 0x02, 24},
    {TESTED_VERSION, 0x2623, 0x2F, 48},
};
const ChargerProfile *charger_profile(const uint8_t *p, size_t k) {
    if (k < 3 + sizeof TESTED_VERSION) return nullptr;
    for (const ChargerProfile &profile : CHARGER_PROFILES)
        if (!memcmp(p + 3, profile.version, sizeof TESTED_VERSION)) return &profile;
    return nullptr;
}
constexpr int64_t LINK_STALE_US = 5000000, VERSION_RECHECK_US = 30000000;
constexpr int64_t ESS_LAYOUT_RETRY_US = 5000000;
constexpr int64_t INSPECTION_TIMEOUT_US = 15000000, INSPECTION_FRESH_US = 300000000;
constexpr uint8_t INSPECTION_SETTING_IDS[] = {2, 3, 4, 6, 64};
constexpr int INSPECTION_SETTINGS = sizeof INSPECTION_SETTING_IDS;
// The flag word's bits the ESS settings set: no charge, no feed-in, no DC-PV feed-in. The ESS assistant
// VEConfigure 3 migrated on 2026-10-27 also sets bits 12 and 13 (Venus read 0x3000, wrote 0x3004); those,
// like any other, are left as read.
constexpr int ESS_FLAG_BITS = 0x07;
const char *const KIND_NAME[] = {"read",          "check",      "write",        "snapshot", "dc-info",
                                 "l1-info",       "flags",      "snap-take",    "flags-write", "charge-state", "ac-version", "ac-write", "ac-status", "operate", "pv-write",
                                 "charger-version", "charger-unit", "charger-info", "charger-read", "charger-write", "soc-sync", "extra-current", "serial",
                                 "version",       "setting-info", "setting",    "unit",     "0x3c",
                                 "grid-info",     "assistants", "meter-info", "meter-read", "fault-read", "access", "setting-write", "unit-write", "record-write",
                                 "reset",         "start-up", "alarm-capability", "alarm-status", "ess-unit", "ess-registers"};
const char *const WRITE_STEP_NAME[] = {"authorizing", "settings", "assistant flag off", "assistant records", "assistant flag on",
                                       "reset",    "waiting for the restart", "start-up", "done"};

// The start-up an MK3 went through after the Multi's reset (VRM's upload, 2026-10-27), and the pause
// after each frame. The sixth goes to the unit's ID; the fifth is answered by the unit.
struct StartupFrame {
    uint8_t bytes[7], n;
    bool to_unit, reply;
    int64_t wait_us;
};
const StartupFrame STARTUP[] = {
    {{0x21, 0xFF, 0xFF, 0xFF, 0xFF, 0x80}, 6, false, false, 1000000},
    {{0x21, 0xFF, 0xFF, 0xFF, 0xFF, 0x83}, 6, false, false, 2000000},
    {{0x21, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00}, 7, false, false, 0},
    {{0x21, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00}, 7, false, false, 0},
    {{0x21, 0x00, 0x00, 0x00, 0x00, 0x00}, 6, false, true, 0},
    {{0x21, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01}, 7, true, false, 0},
    {{0x21, 0x00, 0x00, 0x00, 0x00, 0x00}, 6, false, false, 1000000},
    {{0x21, 0x00, 0x00, 0x00, 0x00, 0x00}, 6, false, false, 0},
};
constexpr int STARTUP_FRAMES = sizeof STARTUP / sizeof STARTUP[0];
const char *const PART_NAME[] = {"serial number", "firmware", "unit", "settings", "0x3C", "grid info",
                                 "assistants",    "done"};
const uint8_t BLOCK_ARG[Requester::BLOCK_ARGS] = {0x00, 0x01, 0x3C, 0x3D, 0x52};  // mk2vsc's, in its order
// Why a part of the settings read was given up, when three tries went unanswered.
const char *const PART_LOST[] = {"the serial number went unanswered", "the firmware went unanswered",
                                 "the unit query (21 .. 05) went unanswered", "", "Winmon 0x3C went unanswered",
                                 "the grid info records went unanswered", "the assistant records went unanswered"};
const char *const CTL_NAME[] = {"off", "testing", "listening", "paused", "backing off", "polling", "checking", "writing"};

bool from_multi(const uint8_t *raw) { return raw[0] == 0x83 && raw[1] == 0x83; }
bool is_sync(const uint8_t *raw, size_t n) { return n >= 6 && from_multi(raw) && raw[2] == 0xFD && raw[4] == 0x55; }
unsigned u16(const uint8_t *p, size_t i) { return p[i] | p[i + 1] << 8; }
uint8_t setting_id(const Requester::SettingsRead &r) {
    return r.inspection ? INSPECTION_SETTING_IDS[r.step] : (uint8_t)r.step;
}
bool legacy_export_format(const Requester::SettingsRead &r) {
    // The genuine 2628459/model47 schema2 export omits the grid field.
    // VEConfigure accepts that format with either eight or ten stored version
    // bytes. Choose that verified file format before probing grid metadata;
    // this does not claim Winmon 0x57 is unsupported and is never a timeout fallback.
    constexpr uint8_t prefix[] = {0x6B, 0x1B, 0x28, 0x00, 0x08, 0x0D, 0x00, 0x00};
    return !r.inspection && r.firmware == 2628459 && r.unit_ok && r.unit_info[0] == 47 &&
           (r.version_bytes == sizeof prefix || r.version_bytes == sizeof r.version) &&
           !memcmp(r.version, prefix, sizeof prefix);
}
const char *mode_name(Requester::Mode m) {
    switch (m) {
    case Requester::Mode::READ: return "on";
    case Requester::Mode::BENCH: return "bench";
    case Requester::Mode::ESS: return "ess";
    default: return "off";
    }
}

}  // namespace

void Requester::note(const char *fmt, ...) {
    size_t room = sizeof notes_ - notes_len_;
    if (room <= 1) return;
    va_list a;
    va_start(a, fmt);
    int n = vsnprintf(notes_ + notes_len_, room, fmt, a);
    va_end(a);
    if (n > 0) notes_len_ += (size_t)n < room ? (size_t)n : room - 1;
}

bool Requester::take_report(char *out, size_t cap) {
    if (notes_len_ == 0 || cap == 0) return false;
    size_t n = notes_len_ < cap - 1 ? notes_len_ : cap - 1;
    memcpy(out, notes_, n);
    out[n] = 0;
    notes_len_ = 0;
    return true;
}

// ---------------------------------------------------------------- test runs

void Requester::start(int64_t now, int64_t seconds, Mode mode, int watts) {
    if(pv_.supported && (pv_.wanted || pv_.actual(now)!=0)){note("# test refused: restore PV operation first\n");return;}
    if (ac_input_.wanted || ac_input_.actual(now)==1) { note("# test refused: reconnect AC input first\n"); return; }
    if (seconds <= 0 || mode == Mode::OFF) {
        stop("switched off");
        return;
    }
    if (write_.active || operation_.active() || charger_.active() || fault_report_.active()) {
        note("# tx refused: inverter control or configuration is busy\n");
        return;
    }
    if (mode == Mode::ESS) {
        if (watts < -MAX_ESS_WATTS || watts > MAX_ESS_WATTS) {
            note("# ess refused: %+d W is outside -%d..+%d W\n", watts, MAX_ESS_WATTS, MAX_ESS_WATTS);
            return;
        }
        if (other_end_ != NONE && now - other_end_ < OTHER_MASTER_QUIET_US) {
            note("# ess refused: another master (the MK3?) was heard %.1f s ago\n", (now - other_end_) / 1e6);
            return;
        }
    }
    stop("restarted");
    int64_t cap = mode == Mode::BENCH ? MAX_BENCH_S : MAX_TX_S;
    if (seconds > cap) seconds = cap;
    on_ = true;
    mode_ = mode;
    watts_ = watts;
    until_ = now + seconds * 1000000;
    why_[0] = 0;
    stop_why_ = nullptr;
    stats_ = TxStats();
    unanswered_in_a_row_ = unacked_in_a_row_ = 0;
    next_due_ = 0;
    if (mode == Mode::ESS) reset_ess_layout();
    last_write_ = NONE;
    switch (mode) {
    case Mode::BENCH:
        note("# tx bench for %lld s: a read a second without a Multi, only while no one else is heard\n",
             (long long)seconds);
        break;
    case Mode::ESS:
        note("# ess on for %lld s: request %+d W every 2.5 s after checking the inverter and discovering its ESS registers\n",
             (long long)seconds, watts);
        break;
    default:
        note("# tx on for %lld s: ReadRAMVar 13+0 at most once a second, after a sync, on a quiet bus, in a cycle "
             "no other master has claimed\n",
             (long long)seconds);
        break;
    }
}

void Requester::stop(const char *why) {
    if (!on_) return;
    on_ = false;
    snprintf(why_, sizeof why_, "%s", why);
    const TxStats &s = stats_;
    note("# %s off (%s): sent=%lu echo_ok=%lu counted=%lu failed=%lu answered=%lu same_cycle=%lu unanswered=%lu "
         "writes=%lu acked=%lu yielded=%lu busy=%lu late=%lu bad=%lu gaps=%lu\n",
         mode_ == Mode::ESS ? "ess" : "tx", why, (unsigned long)s.sent, (unsigned long)s.echo_ok,
         (unsigned long)s.counted, (unsigned long)s.failed, (unsigned long)s.answered, (unsigned long)s.same_cycle,
         (unsigned long)s.unanswered, (unsigned long)s.writes, (unsigned long)s.acked, (unsigned long)s.yielded,
         (unsigned long)s.busy, (unsigned long)s.late, (unsigned long)s.bad, (unsigned long)s.gaps);
    if (mode_ == Mode::ESS && last_write_ != NONE) {
        note("# ess: no more setpoint writes; the Multi falls back to passthrough about 13.6 s after the last\n");
    }
    if (mode_ == Mode::ESS) reset_ess_layout();
}

bool Requester::test_job(int64_t now, Kind *kind) {
    if (now >= until_ || now < next_due_) return false;
    if (mode_ == Mode::ESS) {
        if (const char *gate = ess_firmware_gate(now)) {
            if (inverter_firmware_ && !write_profile_) {
                stop(gate);
                return false;
            }
            if (now < next_firmware_probe_) return false;
            *kind = AC_VERSION;
        } else if (now >= next_firmware_probe_) {
            *kind = AC_VERSION;
        } else {
            *kind = ess_setpoint_ram() ? WRITE : ess_layout_job();
        }
    } else {
        *kind = READ;
    }
    return true;
}

// ---------------------------------------------------------------- the controller

void Requester::control(bool enable) {
    if(!enable && pv_.supported && (pv_.wanted || pv_.actual(now_)!=0)){note("# control: restore PV before disabling bus control\n");return;}
    if (!enable && ac_input_.enabled && (ac_input_.wanted || ac_input_.actual(now_)==1)) {
        note("# control: reconnect the grid before disabling bus control\n"); return;
    }
    ctl_enabled_ = enable;
    if (!enable) reset_ess_layout();
}

inverter::View Requester::inverter_view(int64_t now, const State &st) const {
    CtlState s = control_state(now);
    bool master = (s == CtlState::POLLING || s == CtlState::CHECKING || s == CtlState::WRITING) &&
                  !write_.active && !settings_.active && !charger_.active() && !meter_probe_.active && !fault_report_.active();
    const char *gate = firmware_gate(now);
    inverter::View v = inverter::view(st, operation_, master && !gate, now);
    if (gate && master) snprintf(v.reason, sizeof v.reason, "%s", gate);
    if (st.actual_mode_t < controls_invalidated_at_ || st.led_t < controls_invalidated_at_) v.mode_adjustable = false;
    if (st.limit_a.t < controls_invalidated_at_ || st.led_t < controls_invalidated_at_) v.current_adjustable = false;
    if (st.charge_state_t < controls_invalidated_at_) v.can_equalize = v.can_interrupt = false;
    return v;
}

bool Requester::operate(int64_t now, const inverter::Command &c, const State &st, char *why, size_t cap) {
    if(pv_.supported && (pv_.wanted || pv_.actual(now)!=0) && c.action!=inverter::Action::CURRENT){snprintf(why,cap,"Restore PV operation before operating mode or maintenance changes");return false;}
    if (ac_input_.enabled && (ac_input_.wanted || ac_input_.actual(now)==1) && c.action!=inverter::Action::CURRENT) {
        snprintf(why,cap,"Reconnect the grid before changing operating mode or maintenance");return false;
    }
    if (!inverter::validate(c, inverter_view(now, st), why, cap)) return false;
    operation_ = inverter::Job();
    operation_.command = c;
    operation_.started = now;
    operation_.progress = inverter::Progress::QUEUED;
    snprintf(operation_.message, sizeof operation_.message, "Queued; waiting for a free bus cycle");
    if (c.action == inverter::Action::RESTART || c.action == inverter::Action::REDETECT) {
        // Reuse the captured reset/start-up sequence, without writing settings or assistants.
        plan_.settings = plan_.records = 0;
        plan_.assistants = false;
        memset(plan_.unit_id, 0, sizeof plan_.unit_id);
        write_ = WriteJob();
        write_.active = write_.maintenance = true;
        write_.started = now;
        write_.step = c.action == inverter::Action::RESTART ? W_RESET : W_STARTUP;
        note("# inverter: %s requested\n", c.action == inverter::Action::RESTART ? "restart" : "redetect");
    }
    return true;
}

void Requester::operation_end(bool ok, const char *message) {
    operation_.progress = ok ? inverter::Progress::CONFIRMED : inverter::Progress::FAILED;
    snprintf(operation_.message, sizeof operation_.message, "%s", message);
    note("# inverter: %s\n", message);
}

void Requester::observe(const State &st, int64_t now) {
    if (!operation_.active() || operation_.progress != inverter::Progress::VERIFYING || !st.bus_fresh(now)) return;
    const inverter::Command &c = operation_.command;
    if (c.action == inverter::Action::MODE && operation_.sends >= 2 && st.actual_mode_t > operation_.sent_at && st.actual_mode == c.value)
        operation_end(true, "Operating mode confirmed by the inverter");
    if (c.action == inverter::Action::CURRENT && st.limit_a.t > operation_.sent_at &&
        fabsf(st.limit_a.v * 10 - c.value) < 0.5f)
        operation_end(true, "AC input current limit confirmed by the inverter");
    int wanted = c.action == inverter::Action::EQUALIZE ? 7 : c.action == inverter::Action::ABSORPTION ? 6 : 3;
    if ((c.action == inverter::Action::EQUALIZE || c.action == inverter::Action::ABSORPTION || c.action == inverter::Action::FLOAT) &&
        st.charge_state_t > operation_.sent_at + 200000 && st.charge_main_state == 9 &&
        (st.charge_state == wanted || (c.action == inverter::Action::ABSORPTION && st.charge_state == 2)))
        operation_end(true, "Charger action confirmed by the inverter");
}

bool Requester::charger_ready(int64_t now, char *why, size_t cap) const {
    if(pv_.supported && (pv_.wanted || pv_.actual(now)!=0)){snprintf(why,cap,"Restore PV operation before charger configuration");return false;}
    CtlState state = control_state(now);
    const char *problem = state != CtlState::POLLING && state != CtlState::CHECKING && state != CtlState::WRITING
                              ? "S3 bus control is unavailable"
                          : write_.active || settings_.active || operation_.active() || charger_.active() || meter_probe_.active || fault_report_.active()
                              ? "Wait for the current inverter operation to finish"
                          : sync_t0_ < 0 || now < sync_t0_ || now - sync_t0_ >= LINK_STALE_US
                              ? "No recent inverter data" : nullptr;
    if (problem && cap) snprintf(why, cap, "%s", problem);
    return problem == nullptr;
}

const char *Requester::charger_gate(int64_t now, charger::Profile profile) const {
    if (profile == charger::Profile::ASSIST) return firmware_gate(now);
    if (!inverter_firmware_ || sync_t0_ < 0 || now < sync_t0_ || now - sync_t0_ >= LINK_STALE_US)
        return "Waiting for verified inverter firmware";
    if (!charger_voltage_) return "Stored charger writes are not verified for this inverter firmware";
    if (!charger_unit_known_) return "Read the inverter identity and charger settings before saving";
    return nullptr;
}

void Requester::charger_begin(int64_t now, const charger::Change *change, charger::Profile profile) {
    charger_generation_++;
    // Preserve last known readbacks while refreshing; never turn an edit into an observed value.
    charger_saved_[(unsigned)charger_.profile] = charger_;
    charger::Status next;
    next.profile = profile;
    const auto &previous = charger_saved_[(unsigned)profile];
    next.nominal_voltage = previous.nominal_voltage;
    next.product_id = previous.product_id;
    next.firmware = previous.firmware;
    for (int i = 0; i < charger::count(profile); i++) next.parameter[i] = previous.parameter[i];
    next.stage = charger::Stage::READING;
    next.started = now;
    next.saving = change != nullptr;
    if (change) {
        charger_change_ = *change;
        memcpy(next.requested, change->raw, sizeof next.requested);
    }
    snprintf(next.message, sizeof next.message, "%s", change ? "Reading the inverter before saving" :
             profile == charger::Profile::ASSIST ? "Reading PowerAssist settings from the inverter" : "Reading charger settings from the inverter");
    charger_ = next;
    charger_index_ = charger_cursor_ = charger_tries_ = 0;
    charger_info_ = true;
    charger_version_pending_ = true;
    charger_unit_pending_ = false;
    charger_write_info_checked_ = false;
}

bool Requester::read_charger(int64_t now, char *why, size_t cap, charger::Profile profile) {
    if (!charger_ready(now, why, cap)) return false;
    charger_begin(now, nullptr, profile);
    return true;
}

void Requester::watch_charger(int64_t now) {
    if (charger_voltage_ && !charger_.active() && now >= charger_next_read_) {
        char why[1];
        read_charger(now, why, sizeof why);
    }
}

bool Requester::set_charger(int64_t now, const charger::Change &change, char *why, size_t cap, charger::Profile profile) {
    if (!charger_ready(now, why, cap)) return false;
    if (const char *gate = charger_gate(now, profile)) {
        if (cap) snprintf(why, cap, "%s", gate);
        return false;
    }
    // Validate once against the cached form, and again against a fresh pre-read before any write.
    uint8_t order[charger::COUNT];
    const charger::Status &cached = charger_.profile == profile ? charger_ : charger_saved_[(unsigned)profile];
    for (int i = 0; i < charger::count(profile); i++) {
        const charger::Parameter &p = cached.parameter[i];
        if (!p.known || p.read_at < 0 || now < p.read_at || now - p.read_at >= 120000000) {
            if (cap) snprintf(why, cap, "Read the stored settings before saving");
            return false;
        }
    }
    if (charger::plan(cached.parameter, change, order, why, cap, profile, cached.nominal_voltage) < 0) return false;
    charger_begin(now, &change, profile);
    return true;
}

charger::View Requester::charger_view(int64_t now, charger::Profile profile) const {
    charger::View v;
    v.status = charger_.profile == profile ? charger_ : charger_saved_[(unsigned)profile];
    v.status.profile = profile;
    v.now = now;
    v.available = charger_ready(now, v.reason, sizeof v.reason);
    const char *gate = charger_gate(now, profile);
    if (v.available && gate) {
        v.available = false;
        snprintf(v.reason, sizeof v.reason, "%s", gate);
    }
    if (v.available) {
        for (int i = 0; i < charger::count(profile); i++) {
            const charger::Parameter &p = v.status.parameter[i];
            if (!charger::schema_ok(i, p, profile, v.status.nominal_voltage) || !p.known ||
                p.read_at < 0 || now < p.read_at || now - p.read_at >= 120000000) {
                v.available = false;
                snprintf(v.reason, sizeof v.reason, "%s", profile == charger::Profile::ASSIST ?
                         "Read supported PowerAssist settings from the inverter first" : "Read supported charger settings from the inverter first");
                break;
            }
        }
        if (v.available) snprintf(v.reason, sizeof v.reason, "%s", profile == charger::Profile::ASSIST ?
                                  "Ready to edit stored PowerAssist settings" : "Ready to edit stored charger settings");
    }
    return v;
}

bool Requester::charger_job(Kind *kind) const {
    if (!charger_.active()) return false;
    if (charger_version_pending_) *kind = CHARGER_VERSION;
    else if (charger_unit_pending_) *kind = CHARGER_UNIT;
    else if (charger_.stage == charger::Stage::WRITING)
        *kind = charger_.profile == charger::Profile::CHARGER && !charger_write_info_checked_ ? CHARGER_INFO : CHARGER_WRITE;
    else *kind = charger_.stage == charger::Stage::VERIFYING || !charger_info_ ? CHARGER_READ : CHARGER_INFO;
    return true;
}

void Requester::charger_end(bool ok, const char *message) {
    charger_.stage = ok ? charger::Stage::DONE : charger::Stage::FAILED;
    charger_.finished = now_;
    // An explicit Assist read must not postpone the charger's own cached refresh.
    if (charger_.profile == charger::Profile::CHARGER) charger_next_read_ = now_ + 60000000;
    snprintf(charger_.message, sizeof charger_.message, "%s", message);
    note("# charger: %s (writes %d, readback confirmations %d)\n", message, charger_.writes, charger_.confirmations);
}

void Requester::invalidate_charger(const char *message, bool retry_soon) {
    if (charger_.active()) charger_end(false, message);
    charger_next_read_ = retry_soon ? 0 : now_ + 60000000;
    charger_unit_known_ = false;
    for (auto *cached : {&charger_, &charger_saved_[0], &charger_saved_[1]}) {
        cached->nominal_voltage = 0;
        cached->product_id = 0;
        cached->firmware = 0;
        for (auto &p : cached->parameter) p.known = false;
    }
}

void Requester::invalidate_inverter_configuration(const char *message, bool break_frame) {
    // Both recorded transfer breaks followed a matching Enable ACK by about
    // 23 ms, with regular syncs continuing.
    const bool ignore_transfer = break_frame && runtime_firmware_verified() && ac_input_.wanted && ac_input_.ack &&
        ac_input_.sent_value == 1 && ac_input_.sent_at >= 0 && now_ >= ac_input_.sent_at &&
        now_ - ac_input_.sent_at < 200000 && sync_t0_ >= 0 && now_ >= sync_t0_ && now_ - sync_t0_ < 50000;
    invalidate_runtime_controls(message, ignore_transfer);
    invalidate_charger(message);
    if (settings_.active || settings_.done) {
        settings_.active = false;
        settings_.done = true;
        settings_.finished = now_;
        settings_.problem = message;
    }
    if (!write_.active || write_.maintenance || write_.reset_sends) return;
    const bool marking = write_.step == W_MARK && kind_ == UNIT_WRITE && write_.config_sends;
    const bool marked = write_marked() && write_.step <= W_UNMARK;
    // Marking the inverter to stop its assistants produces this break.
    if (break_frame && (marking || marked)) return;
    write_end(marking || marked ? "Assistant write interrupted; recover the inverter configuration before trying again" : message);
}

void Requester::invalidate_runtime_controls(const char *message, bool preserve_ignore_ac) {
    if (settings_.inspection && (settings_.active || (settings_.done && !settings_.problem)))
        settings_finish(message);
    inverter_firmware_ = 0;
    write_profile_ = nullptr;
    ess_discovery_supported_ = false;
    charger_voltage_ = 0;
    next_firmware_probe_ = 0;
    controls_invalidated_at_ = now_;
    ctl_armed_ = false;
    ctl_flags_ = -1;
    ctl_watts_ = 0;
    reset_ess_layout();
    external_soc_valid_ = extra_current_pending_ = false;
    extra_current_raw_ = 0;
    if (preserve_ignore_ac) {
        ignore_ac_transfer_until_ = ac_input_.sent_at + 3000000;
        ac_input_.status_at = -1;
        ac_input_.raw = -1;
        ac_input_.verified = false;
        ac_input_.next_status = 0;
    } else {
        ignore_ac_transfer_until_ = -1;
        ac_input_.owner_lost();
    }
    pv_.owner_lost();
    if (on_ && mode_ == Mode::ESS) stop(message);
    if (write_.active && write_.maintenance) {
        // Finish the startup sequence after a reset we already sent.
        if (write_.reset_sends == 0) write_end(message);
    } else if (operation_.active()) {
        operation_end(false, message);
    }
}

bool Requester::runtime_firmware_verified() const {
    return write_profile_ && write_profile_->other_runtime;
}

void Requester::observe_firmware(const uint8_t *p, size_t k) {
    const uint32_t firmware = u16(p, 3) | (uint32_t)u16(p, 5) << 16;
    const auto *writes = match_write_profile(p + 3, k - 3);
    const bool verified = writes && writes->other_runtime;
    const uint32_t family = firmware / 100000;
    // Victron's ESS Assistant supports 26/27 processors from revision 422.
    // The documented long Winmon reply carries four version bytes and two reserved bytes.
    const bool ess_discovery = k >= 9 && (family == 26 || family == 27) && firmware % 1000 >= 422;
    const ChargerProfile *profile = charger_profile(p, k);
    const uint8_t voltage = profile ? profile->voltage : 0;
    if ((inverter_firmware_ && (inverter_firmware_ != firmware || write_profile_ != writes ||
                                (ess_discovery_supported_ && !ess_discovery) ||
                                (charger_voltage_ && charger_voltage_ != voltage))) ||
        (!verified && ignore_ac_transfer_pending(now_))) {
        constexpr char message[] = "Inverter firmware changed; fresh device checks required";
        invalidate_runtime_controls(message);
        invalidate_charger(message);
    }
    if (!ac_input_.version_known || ac_input_.firmware != firmware || runtime_firmware_verified() != verified) {
        ac_input_.reply(acinput::Wire::VERSION, p, k, now_);
        if (!verified) {
            ac_input_.supported = false;
            ac_input_.error = "Writes are not verified for this inverter firmware";
        }
    }
    inverter_firmware_ = firmware;
    write_profile_ = writes;
    ess_discovery_supported_ = ess_discovery;
    charger_voltage_ = voltage;
    next_firmware_probe_ = now_ + VERSION_RECHECK_US;
}

void Requester::reset_ess_layout() {
    control_generation_++;
    ess_discovered_ram_ = ess_header_ = ess_candidate_ = 0;
    ess_chain_complete_ = ess_absent_ = false;
    ess_retry_at_ = 0;
    multi_flags_ = -1;
    ctl_written_w_ = NOT_WRITTEN;
    ctl_next_[WRITE] = ctl_next_[FLAGS] = ctl_next_[FLAGS_WRITE] = 0;
}

bool Requester::ess_discovery_available(int64_t now) const {
    return ess_discovery_supported_ && sync_t0_ >= 0 && now >= sync_t0_ && now - sync_t0_ < LINK_STALE_US;
}

uint8_t Requester::ess_discovered_ram(int64_t now) const {
    return ess_discovery_available(now) ? ess_discovered_ram_ : 0;
}

Requester::Kind Requester::ess_layout_job() const {
    if (!ess_header_) return ESS_UNIT;
    return ess_chain_complete_ ? ESS_REGISTERS : CHECK;
}

const char *Requester::ess_layout_reply(const uint8_t *p, size_t k) {
    if (kind_ == ESS_UNIT) {
        constexpr uint8_t standalone[] = {1, 1, 1, 0, 0, 0, 0};
        constexpr uint8_t broadcast[] = {0, 0, 0, 0};
        // Preserve the 2623563 check. Other write profiles also require a zero unit role nibble.
        if (!p || k < 15 || !memcmp(p + 1, broadcast, sizeof broadcast) ||
            !p[5] || memcmp(p + 6, standalone, sizeof standalone) || p[14] != 0 ||
            (write_profile_ && (p[5] != write_profile_->model ||
                                (!write_profile_->other_runtime && (p[13] & 0x0F)))))
            return "ESS requires a recognized standalone VE.Bus unit layout";
        ess_header_ = ASSISTANTS;
        return nullptr;
    }
    if (!p || k < 7 || p[2] != 0x85) return "ESS layout read was missing or refused";
    if (kind_ == ESS_REGISTERS) {
        ess_discovered_ram_ = ess_candidate_;
        if (ess_setpoint_ram()) {
            multi_flags_ = u16(p, 5);
            if (runtime_firmware_verified() && pv_.enabled) pv_.observe(multi_flags_, now_);
            ctl_next_[WRITE] = 0;
        }
        note("# ess: setpoint RAM %u and flags RAM %u checked on firmware %lu\n",
             unsigned(ess_discovered_ram_), unsigned(ess_discovered_ram_ + 1), (unsigned long)inverter_firmware_);
        return nullptr;
    }
    const unsigned header = u16(p, 3);
    if (!header) {
        if (!ess_candidate_) {
            ess_absent_ = true;
            note("# ess: no ESS Assistant found in the live layout\n");
            return nullptr;
        }
        ess_chain_complete_ = true;
        return nullptr;
    }
    const unsigned size = header & 0x0f;
    const unsigned next = ess_header_ + 1u + size;
    if (next > UINT8_MAX) return "ESS Assistant records exceed the RAM address range";
    if (header >> 4 == ESS_ASSISTANT_ID) {
        if (size < 2 || ess_candidate_) return "ESS Assistant layout is short or has duplicate records";
        ess_candidate_ = ess_header_ + 1;
    }
    ess_header_ = next;
    return nullptr;
}

void Requester::charger_reply(const uint8_t *p, size_t k) {
    if (!charger_.active() || sent_charger_generation_ != charger_generation_) return;
    if (kind_ == CHARGER_VERSION) {
        if (!p || k < 7 || p[2] != 0x82) {
            if (++charger_tries_ >= 2) {
                const char *message = "Inverter firmware could not be verified; read the settings again before saving";
                invalidate_runtime_controls(message);
                invalidate_charger(message, false);
            }
            return;
        }
        observe_firmware(p, k);
        if (!charger_.active()) return;
        if (charger_.saving) {
            if (const char *gate = charger_gate(now_, charger_.profile)) {
                charger_end(false, gate);
                return;
            }
        }
        const ChargerProfile *profile = charger_profile(p, k);
        charger_.firmware = inverter_firmware_;
        charger_.product_id = profile ? profile->product_id : 0;
        charger_model_ = profile ? profile->model : 0;
        charger_unit_pending_ = profile && charger_.profile == charger::Profile::CHARGER;
        if (charger_.profile == charger::Profile::ASSIST) charger_.nominal_voltage = runtime_firmware_verified() ? 48 : 0;
        charger_tries_ = 0;
        charger_version_pending_ = false;
        return;
    }
    if (kind_ == CHARGER_UNIT) {
        if (!p) {
            if (++charger_tries_ >= 2) invalidate_charger("Inverter unit identity could not be verified", false);
            return;
        }
        // The model byte and the following phase/unit bytes must match the
        // standalone layout recorded in all three supported profiles.
        constexpr uint8_t single_unit[] = {1, 1, 1, 0, 0, 0, 0};
        constexpr uint8_t broadcast[] = {0, 0, 0, 0};
        if (p[5] != charger_model_ || memcmp(p + 6, single_unit, sizeof single_unit) ||
            !memcmp(p + 1, broadcast, sizeof broadcast)) {
            invalidate_charger("Inverter model or unit layout does not match the charger profile", false);
            return;
        }
        if (charger_unit_known_ && memcmp(p + 1, charger_unit_, sizeof charger_unit_)) {
            invalidate_charger("Inverter unit changed; read the charger settings again");
            return;
        }
        memcpy(charger_unit_, p + 1, sizeof charger_unit_);
        charger_unit_known_ = true;
        charger_.nominal_voltage = charger_voltage_;
        charger_unit_pending_ = false;
        charger_tries_ = 0;
        return;
    }
    charger::Parameter &s = charger_.parameter[charger_index_];
    if (kind_ == CHARGER_WRITE) {
        if (p && (k < 3 || p[2] != 0x88)) {
            charger_end(false, "The inverter refused a charger-setting write; reload to check stored values");
            return;
        }
        charger_.acknowledged[charger_index_] = p && k >= 3 && p[2] == 0x88;
        // Even after a lost ACK, read the setting instead of repeating an EEPROM write.
        charger_.stage = charger::Stage::VERIFYING;
        charger_tries_ = 0;
        snprintf(charger_.message, sizeof charger_.message, "Reading the setting back after writing");
        return;
    }
    bool ok = p && (kind_ == CHARGER_INFO ? k >= 14 && p[2] == 0x89 : k >= 5 && p[2] == 0x86);
    if (!ok) {
        if (++charger_tries_ < 2) return;
        if (kind_ == CHARGER_INFO) {
            invalidate_charger("Charger setting limits could not be verified; read the settings again", false);
        } else {
            invalidate_charger("Charger setting could not be read; reload to check stored values", false);
        }
        return;
    }
    charger_tries_ = 0;
    if (kind_ == CHARGER_INFO) {
        charger::Parameter live = s;
        live.info = true;
        live.scale = (int16_t)u16(p, 3); live.offset = (int16_t)u16(p, 5);
        live.minimum = (uint16_t)u16(p, 9); live.maximum = (uint16_t)u16(p, 11); live.access = p[13];
        if (charger_.stage == charger::Stage::WRITING) {
            if (live.scale != s.scale || live.offset != s.offset || live.minimum != s.minimum ||
                live.maximum != s.maximum || live.access != s.access) {
                invalidate_charger("A live charger setting limit changed; read the settings again", false);
                return;
            }
            charger_write_info_checked_ = true;
            return;
        }
        s = live;
        if ((charger_.nominal_voltage || charger_.profile == charger::Profile::ASSIST) &&
            !charger::schema_ok(charger_index_, s, charger_.profile, charger_.nominal_voltage)) {
            s.known = false;
            charger_end(false, "Unsupported or access-protected charger setting; use VEConfigure");
            return;
        }
        charger_info_ = false;
        return;
    }
    s.raw = (uint16_t)u16(p, 3);
    s.known = true;
    s.read_at = now_;
    if (charger_.stage == charger::Stage::VERIFYING) {
        if (s.raw != charger_change_.raw[charger_index_]) {
            charger_end(false, "Write readback differs from the requested value; stopped without changing further settings");
            return;
        }
        charger_.verified[charger_index_] = true;
        charger_.confirmations++;
        note("# charger: setting %u = %u read back%s\n", charger::setting_id(charger_index_, charger_.profile), s.raw,
             charger_.acknowledged[charger_index_] ? " after WriteSettingOK" : " (write ACK not received)");
        if (++charger_cursor_ >= charger_.changes) {
            charger_end(true, charger_.profile == charger::Profile::ASSIST ?
                        "Saved PowerAssist settings confirmed by inverter readback" : "Saved charger settings confirmed by inverter readback");
            return;
        }
        charger_index_ = charger_order_[charger_cursor_];
        charger_.stage = charger::Stage::WRITING;
        charger_version_pending_ = true;
        charger_write_info_checked_ = false;
        return;
    }
    if (++charger_index_ < charger::count(charger_.profile)) { charger_info_ = true; return; }
    if (!charger_.saving) { charger_end(true, charger_.profile == charger::Profile::ASSIST ?
        "PowerAssist settings read from the inverter" : "Charger settings read from the inverter"); return; }
    char why[160];
    int changes = charger::plan(charger_.parameter, charger_change_, charger_order_, why, sizeof why,
                                charger_.profile, charger_.nominal_voltage);
    if (changes < 0) { charger_end(false, why); return; }
    charger_.changes = changes;
    for (int i = 0; i < charger::count(charger_.profile); i++) charger_.verified[i] = charger_.parameter[i].raw == charger_change_.raw[i];
    if (!changes) { charger_end(true, "Already set: fresh inverter readback matches; no EEPROM writes"); return; }
    charger_cursor_ = 0;
    charger_index_ = charger_order_[0];
    charger_.stage = charger::Stage::WRITING;
    charger_version_pending_ = true;
    charger_write_info_checked_ = false;
    snprintf(charger_.message, sizeof charger_.message, "Saving changed charger settings");
}

void Requester::target(bool write, int watts, int flags) {
    if (write && !ctl_armed_) {
        last_write_ = NONE;
        ctl_unacked_in_a_row_ = 0;
        ctl_next_[WRITE] = 0;  // in the next free cycle
        ctl_next_[FLAGS] = 0;  // and read the flag word soon
        ctl_armed_ = true;
        note("# control: ESS from here on: the setpoint is written whenever no other master is on the bus\n");
    } else if (!write && ctl_armed_) {
        ctl_armed_ = false;
        note("# control: ESS from here off: no more setpoint writes%s\n",
             ctl_last_write_ != NONE ? "; the Multi falls back to passthrough about 13.6 s after the last" : "");
    }
    ctl_watts_ = watts < -MAX_CTL_WATTS ? -MAX_CTL_WATTS : watts > MAX_CTL_WATTS ? MAX_CTL_WATTS : watts;
    if (flags != ctl_flags_) {
        ctl_flags_ = flags;
        if (flags >= 0 && ctl_armed_) note("# control: the flag word should be 0x%02X\n", flags);
    }
}

template <int Capacity> static void clear_records(Requester::Records<Capacity> &x) {
    memset(x.len, 0, sizeof x.len);
    x.count = 0;
    x.ended = false;
}

bool Requester::read_fault_report(int64_t now, char *why, size_t cap) {
    CtlState s=control_state(now);
    const char *problem=nullptr;
    if(s!=CtlState::POLLING && s!=CtlState::CHECKING && s!=CtlState::WRITING)
        problem="Wait for brain_zero to own the VE.Bus; disconnect the MK3 first";
    else if(settings_busy() || meter_probe_.active || on_)
        problem="Wait for the current inverter operation to finish";
    else if(sync_t0_<0 || now<sync_t0_ || now-sync_t0_>=5000000)
        problem="No recent inverter data";
    else if(!ac_input_.version_known || ac_input_.firmware!=faults::TESTED_FIRMWARE)
        problem="Detailed fault reads are not yet validated for this inverter firmware";
    else if(fault_report_.started>=0 && now-fault_report_.started<faults::COOLDOWN_US)
        problem="Wait 30 seconds between fault report reads";
    if(problem){snprintf(why,cap,"%s",problem);return false;}
    fault_report_.begin(now,ac_input_.firmware);
    note("# faults: explicit read-only report queued; ESS retains priority\n");
    return true;
}

bool Requester::probe_meters(int64_t now) {
    CtlState s=control_state(now);
    if(meter_probe_.active||on_||write_.active||settings_.active||operation_.active()||charger_.active()||fault_report_.active()||
       (s!=CtlState::POLLING&&s!=CtlState::CHECKING&&s!=CtlState::WRITING)){
        note("# meters: refused; wait for normal S3 bus control and other operations to finish\n");return false;
    }
    static const uint8_t ids[METER_COUNT]={0,1,2,3,4,5,9,11,13,14,15,16,17,18,19,26,128,130,131,132,133,134};
    meter_probe_=MeterProbe();meter_probe_.active=true;meter_probe_.started=now;meter_generation_++;
    for(int i=0;i<METER_COUNT;i++)meter_probe_.values[i].id=ids[i];
    note("# meters: starting read-only RAM/scale probe; ESS refresh continues\n");
    return true;
}

void Requester::meter_reply(const uint8_t *p,size_t k) {
    if(!meter_probe_.active||sent_meter_generation_!=meter_generation_)return;
    MeterValue &v=meter_probe_.values[meter_probe_.index];
    if(kind_==METER_INFO){
        if(p&&k>=7&&p[2]==0x8E){v.scale=(int16_t)u16(p,3);v.offset=(int16_t)u16(p,5);v.info=v.scale!=0;}
        meter_probe_.info=false;
    }else{
        if(p&&k>=5&&p[2]==0x85){
            v.raw=u16(p,3);v.known=true;
            if(v.info){
                int sc=v.scale<0?-(int)v.scale:v.scale;
                float scale=sc>=0x4000?(sc==0x8000?0:1.0f/(0x8000-sc)):(float)sc;
                if(v.offset==INT16_MIN){if(v.scale>=1&&v.scale<=16)v.value=(v.raw>>(v.scale-1))&1;else v.info=false;}
                else if(sc==0x8000)v.info=false;
                else v.value=scale*((v.scale<0?(int16_t)v.raw:(int)v.raw)+v.offset);
            }
        }
        note("# meter id=%u info=%d sc=%d offset=%d known=%d raw=%u signed=%d value=%.4f\n",v.id,v.info,v.scale,v.offset,v.known,v.raw,(int16_t)v.raw,v.value);
        meter_probe_.info=true;
        if(++meter_probe_.index==METER_COUNT){meter_probe_.active=false;meter_probe_.done=true;note("# meters: read-only probe complete\n");}
    }
}

bool Requester::read_settings(int64_t now, bool inspection) {
    SettingsRead &r = settings_;
    if (r.active || !ctl_enabled_ || on_ || write_.active || operation_.active() || charger_.active() || meter_probe_.active || fault_report_.active()) return false;
    if (inspection && !inspection_link_ready(now)) return false;
    settings_generation_++;
    for (Setting &s : r.s) s = Setting();
    r.active = true;
    r.done = false;
    r.inspection = inspection;
    r.part = SERIAL_PART;
    r.step = 0;
    r.chunk = 1;
    r.info_done = false;
    r.tries = 0;
    r.read = r.unsupported = r.failed = 0;
    r.serial[0] = 0;
    r.firmware = 0;
    memset(r.version, 0, sizeof r.version);
    r.version_bytes = 0;
    r.version_ok = r.unit_ok = false;
    memset(r.unit_id, 0, sizeof r.unit_id);
    memset(r.unit_info, 0, sizeof r.unit_info);
    memset(r.block, 0, sizeof r.block);
    memset(r.block_got, 0, sizeof r.block_got);
    clear_records(r.grid);
    r.grid_availability = GridAvailability::UNKNOWN;
    clear_records(r.assistants);
    r.problem = nullptr;
    r.started = now;
    r.finished = -1;
    if (inspection) {
        note("# settings: reading the connected MultiPlus-II identity and commissioning settings\n");
    } else {
        note("# settings: reading the serial number, the firmware, the unit, settings 0-%d, then Winmon 0x3C, the grid "
             "info and the assistant records, as Venus's backup\n", SETTINGS - 1);
    }
    return true;
}

bool Requester::inspection_link_ready(int64_t now) const {
    const CtlState state = control_state(now);
    return (state == CtlState::POLLING || state == CtlState::CHECKING || state == CtlState::WRITING) &&
           sync_t0_ >= 0 && now >= sync_t0_ && now - sync_t0_ < LINK_STALE_US;
}

bool Requester::inspection_ready(int64_t now) const {
    const SettingsRead &r = settings_;
    return r.inspection && r.done && !r.problem && r.unit_ok && r.serial[0] &&
           r.firmware && r.firmware == inverter_firmware_ && r.started >= controls_invalidated_at_ &&
           r.finished >= 0 && now >= r.finished && now - r.finished < INSPECTION_FRESH_US && inspection_link_ready(now);
}

const char *Requester::part_name(int part) { return part >= 0 && part <= PARTS ? PART_NAME[part] : "?"; }

int Requester::progress(const SettingsRead &r) {
    // By the requests each part takes on this Multi: 3, then 337 for the settings, 5, 169 for 28 grid
    // info records and 200 for 44 assistant records.
    if (r.done) return 100;
    if (!r.active) return 0;
    if (r.inspection) {
        constexpr int total_requests = 3 + 2 * INSPECTION_SETTINGS;
        const int completed = r.part == SETTINGS_PART ? 3 + 2 * r.step + int(r.info_done) : r.part;
        return 100 * completed / total_requests;
    }
    switch (r.part) {
    case SETTINGS_PART: return 1 + 46 * r.step / SETTINGS;
    case BLOCK_PART: return 47;
    case GRID_PART: return 48 + 24 * (r.step < 28 ? r.step : 28) / 28;
    case ASSISTANTS_PART: return 72 + 27 * (r.step < 44 ? r.step : 44) / 44;
    default: return 0;
    }
}

bool Requester::settings_job(Kind *kind) {
    const SettingsRead &r = settings_;
    if (!r.active) return false;
    static const Kind KIND[] = {SERIAL_READ, VERSION_READ, UNIT_READ, INFO_READ, BLOCK_READ, GRID_READ, ASSISTANT_READ};
    *kind = r.part == SETTINGS_PART && r.info_done ? SETTING_READ : KIND[r.part];
    return true;
}

// The current request is done: on to the next setting or 0x3C argument, or the next part. The
// records' parts move on in records_reply().
void Requester::settings_next() {
    SettingsRead &r = settings_;
    r.tries = 0;
    r.info_done = false;
    if (r.part == SETTINGS_PART && ++r.step < (r.inspection ? INSPECTION_SETTINGS : SETTINGS)) return;
    if (r.part == BLOCK_PART && ++r.step < BLOCK_ARGS) return;
    settings_part_done();
}

void Requester::settings_part_done() {
    SettingsRead &r = settings_;
    if (r.inspection && r.part == SETTINGS_PART) {
        settings_finish(r.read ? nullptr : "No commissioning settings could be read");
        return;
    }
    r.tries = 0;
    r.info_done = false;
    r.step = 0;
    r.chunk = 1;
    ++r.part;
    if (r.part == GRID_PART && legacy_export_format(r)) {
        r.grid_availability = GridAvailability::LEGACY_OMITTED;
        ++r.part;  // Schema2 has no grid field. Do not fabricate records or a terminating reply.
    }
    if (r.part < PARTS) return;
    settings_finish(nullptr);
}

void Requester::settings_finish(const char *problem) {
    SettingsRead &r = settings_;
    r.active = false;
    r.done = true;
    r.part = PARTS;
    r.finished = now_;
    if (problem) r.problem = problem;
    note("# settings: %d read, %d not supported, %d failed, in %.1f s (serial %s, firmware %lu); %d grid info and %d "
         "assistant records%s%s\n",
         r.read, r.unsupported, r.failed, (r.finished - r.started) / 1e6, r.serial[0] ? r.serial : "unknown",
         (unsigned long)r.firmware, r.grid.count, r.assistants.count, r.problem ? "; " : "", r.problem ? r.problem : "");
}

void Requester::settings_give_up(const char *why) {
    SettingsRead &r = settings_;
    if (r.inspection) {
        settings_finish(why);
        return;
    }
    if (!r.problem) r.problem = why;
    note("# settings: %s; going on without it\n", why);
    settings_part_done();
}

// A reply to chunk `chunk` of record `step`: 11 bytes, the first 64 of a record's six chunks kept; fewer
// end the record (none at all: a blank one); 0x80 0x02 comes after the last record, and keeps a record it
// cuts short as far as it was read.
template <int Capacity> void Requester::records_reply(Records<Capacity> &x, uint8_t code, const uint8_t *p, size_t k) {
    SettingsRead &r = settings_;
    if (k >= 4 && p[2] == 0x80 && p[3] == 0x02) {
        if (r.chunk > 1) x.count = r.step + 1;
        x.ended = true;
        settings_part_done();
        return;
    }
    if (p[2] != code) {
        if (++r.tries >= 3) settings_give_up(PART_LOST[r.part]);
        return;
    }
    // A full bank can still receive its terminating reply, but cannot store another record.
    if (r.step >= Capacity) {
        settings_give_up("more records than the listener has room for");
        return;
    }
    size_t n = k - 3 < (size_t)CHUNK ? k - 3 : (size_t)CHUNK;
    size_t at = (size_t)(r.chunk - 1) * CHUNK, room = RECORD - at;
    memcpy(x.data[r.step] + at, p + 3, n < room ? n : room);
    x.len[r.step] = (uint8_t)(at + (n < room ? n : room));
    r.tries = 0;
    if (n < (size_t)CHUNK || r.chunk == CHUNKS) {
        x.count = r.step + 1;
        r.chunk = 1;
        ++r.step;
    } else {
        r.chunk++;
    }
}

// A reply to one of the settings read's requests, or none (p null): three tries, then the next setting,
// or the part is given up.
void Requester::settings_reply(const uint8_t *p, size_t k) {
    SettingsRead &r = settings_;
    if (!r.active || sent_settings_generation_ != settings_generation_) return;
    if (!p) {
        if (++r.tries < 3) return;
        if (r.part == SETTINGS_PART) {
            r.s[setting_id(r)].state = READ_FAILED;
            r.failed++;
            settings_next();
        } else {
            settings_give_up(PART_LOST[r.part]);
        }
        return;
    }
    switch (kind_) {
    case SERIAL_READ:  // 0x9D and ASCII, "HQ..."
        if (p[2] == 0x9D) {
            size_t n = 0;
            for (size_t i = 3; i < k && n + 1 < sizeof r.serial; i++) {
                if (p[i] >= 0x20 && p[i] < 0x7F) r.serial[n++] = (char)p[i];
            }
            r.serial[n] = 0;
        }
        if (r.inspection && !r.serial[0]) {
            settings_finish("Could not read the connected MultiPlus-II serial number");
            return;
        }
        settings_next();
        break;
    case VERSION_READ:  // 0x82 and the version bytes actually returned
        if (p[2] == 0x82 && k >= 7) {
            r.firmware = u16(p, 3) | (uint32_t)u16(p, 5) << 16;
            observe_firmware(p, k);
        }
        if (r.inspection && !r.active) return;
        if (r.inspection && (p[2] != 0x82 || k < 9 || !multiplus_ii::find(r.firmware))) {
            settings_finish("Commissioning requires a recognized 230 V MultiPlus-II firmware reply");
            return;
        }
        if (p[2] == 0x82) {
            r.version_bytes = uint8_t(k - 3);
            const size_t stored = r.version_bytes < sizeof r.version ? r.version_bytes : sizeof r.version;
            memcpy(r.version, p + 3, stored);
            r.version_ok = k >= 13;
        }
        settings_next();
        break;
    case UNIT_READ: {  // frame 0x20: the unit's ID and ten bytes (matched in heard())
        constexpr uint8_t standalone[] = {1, 1, 1, 0, 0, 0, 0};
        constexpr uint8_t broadcast[] = {0, 0, 0, 0};
        if (r.inspection && (!memcmp(p + 1, broadcast, sizeof broadcast) || !p[5] ||
                             memcmp(p + 6, standalone, sizeof standalone) || p[14] != 0)) {
            settings_finish("Commissioning requires a recognized standalone VE.Bus unit layout");
            return;
        }
        memcpy(r.unit_id, p + 1, sizeof r.unit_id);
        memcpy(r.unit_info, p + 5, sizeof r.unit_info);
        r.unit_ok = true;
        settings_next();
        break;
    }
    case BLOCK_READ:  // the second reply, 0x9F: the argument and eight bytes, after 0x9E's (heard())
        if (p[2] == 0x9F && k >= 12 && p[3] == BLOCK_ARG[r.step] && (r.block_got[r.step] & 1)) {
            memcpy(r.block[r.step] + 8, p + 4, 8);
            r.block_got[r.step] |= 2;
            settings_next();
        } else if (++r.tries >= 3) {
            settings_give_up("Winmon 0x3C was not answered as mk2vsc's was");
        } else {
            r.block_got[r.step] = 0;  // ask again
        }
        break;
    case GRID_READ:
        if (p[2] == 0xA3 || (k >= 4 && p[2] == 0x80 && p[3] == 0x02))
            r.grid_availability = GridAvailability::AVAILABLE;
        records_reply(r.grid, 0xA3, p, k);
        break;
    case ASSISTANT_READ: records_reply(r.assistants, 0x96, p, k); break;
    case INFO_READ: {  // 0x89: scale, offset, default, minimum, maximum, access; empty if not supported
        Setting &s = r.s[setting_id(r)];
        // MK2 mode 2 includes the access byte. Never invent it for an
        // unqualified inverter or mistake a malformed reply for an absent
        // setting. Extra trailing bytes remain allowed for future extensions.
        if (p[2] == 0x89 && k >= 14) {
            s.scale = (int16_t)u16(p, 3);
            s.offset = (int16_t)u16(p, 5);
            s.def = (uint16_t)u16(p, 7);
            s.min = (uint16_t)u16(p, 9);
            s.max = (uint16_t)u16(p, 11);
            s.access = p[13];
            if (r.inspection && (!s.scale || s.min > s.max)) {
                s.state = READ_FAILED;
                r.failed++;
                settings_next();
                break;
            }
            r.info_done = true;
            r.tries = 0;
        } else if (p[2] == 0x89 && k == 3) {
            s.state = UNSUPPORTED;
            r.unsupported++;
            settings_next();
        } else {
            s.state = READ_FAILED;
            r.failed++;
            settings_next();
        }
        break;
    }
    case SETTING_READ: {  // 0x86 and the value
        Setting &s = r.s[setting_id(r)];
        if (p[2] == 0x86 && k >= 5) {
            s.value = (uint16_t)u16(p, 3);
            if (!r.inspection || (s.value >= s.min && s.value <= s.max)) {
                s.state = READ_OK;
                r.read++;
                settings_next();
                break;
            }
        }
        s.state = READ_FAILED;
        r.failed++;
        settings_next();
        break;
    }
    default:
        break;
    }
}

// ---------------------------------------------------------------- writing a settings file

const char *Requester::write_step_name(int step) { return step >= 0 && step <= W_DONE ? WRITE_STEP_NAME[step] : "?"; }

int Requester::write_progress(const WriteJob &j, const WritePlan &p) {
    if (j.done) return 100;
    if (!j.active) return 0;
    // A setting counts one request, an assistant record six, the reset and start-up together 30.
    int records = p.assistants ? 6 * p.records : 0;
    int auth = (int)p.access.count, total = auth + p.settings + records + (p.restart ? 30 : 0), done = auth;
    switch (j.step) {
    case W_AUTHORIZE: done = j.index; break;
    case W_SETTINGS: done += j.index; break;
    case W_MARK: done += p.settings; break;
    case W_RECORDS: done += p.settings + 6 * (p.records - 1 - j.index) + (j.chunk ? 6 - j.chunk : 5); break;
    case W_UNMARK:
    case W_RESET: done += p.settings + records; break;
    case W_RESTART: done += p.settings + records + 10; break;
    case W_STARTUP: done += p.settings + records + 10 + 2 * j.index; break;
    default: break;
    }
    return total ? 100 * done / total : 0;
}

// Record r's first request: 0x58 when it and the next are blank, else chunk 6. On 2026-10-27 mk2vsc
// blanked records 30-41 this way and wrote record 42, blank but followed by data, as ff.
static uint8_t first_chunk(const Requester::WritePlan &p, int r) {
    bool next_blank = r + 1 >= p.records || p.blank[r + 1];
    return p.blank[r] && next_blank ? 0 : Requester::CHUNKS;
}

bool Requester::start_access_test(int64_t now, const SettingsAccess &access, const char **why, int64_t utc) {
    // Build a separate empty plan. No setting, assistant or restart from the loaded file reaches it.
    static WritePlan test;
    memset(static_cast<void *>(&test), 0, sizeof test);
    test.access_only = true;
    test.read_started = settings_.started;
    test.access = access;
    bool ok = start_write(now, test, why, utc);
    test.access.clear();
    return ok;
}

bool Requester::start_write(int64_t now, const WritePlan &plan, const char **why, int64_t utc) {
    if(pv_.supported && (pv_.wanted || pv_.actual(now)!=0)){if(why)*why="Restore PV operation before writing inverter settings";return false;}
    if(ac_input_.wanted || ac_input_.actual(now)==1) { if(why)*why="Reconnect AC input before writing inverter settings";return false; }
    CtlState s = control_state(now);
    const char *w = write_.active                ? "a settings file is already being written"
                    : meter_probe_.active        ? "a read-only meter probe is running"
                    : fault_report_.active()     ? "a read-only fault report is running"
                    : operation_.active()         ? "an inverter control is pending"
                    : charger_.active()           ? "charger configuration is busy"
                    : !ctl_enabled_              ? "the controller is off (ctl on)"
                    : on_                        ? "a test run is on"
                    : settings_.active           ? "a settings read is running"
                    : s == CtlState::PAUSED      ? "another master (the MK3) is on the bus: stop Venus's VE.Bus service "
                                                   "or unplug the MK3 first"
                    : s == CtlState::LISTENING   ? "still listening for other masters after a restart"
                    : s == CtlState::BACKING_OFF ? "backing off after failed requests"
                    : plan.settings < 0 || plan.settings > SETTINGS ? "bad plan"
                    : plan.access_only && (!plan.access.count || plan.settings || plan.assistants || plan.restart || plan.records || plan.required_access)
                        ? "invalid credentials-only plan"
                    : plan.settings == 0 && !plan.assistants && !plan.access_only ? "nothing to write"
                    : plan.assistants && (plan.records < 1 || plan.records > MAX_RECORDS) ? "no assistant records"
                                                                                          : nullptr;
    if (!w && plan.required_access && !plan.access.count) w = "protected settings require embedded authorization";
    if (!w) w = plan.access.problem(utc);
    if (!w) w = settings_write_firmware_gate(now);
    if (!w) {
        const SettingsRead &r = settings_;
        if (!r.done || r.inspection || r.problem || r.failed || !r.version_ok || !r.unit_ok || !r.serial[0] ||
            r.started < 0 || r.started != plan.read_started || r.started < controls_invalidated_at_ ||
            r.finished < r.started || now < r.finished || now - r.finished >= LINK_STALE_US ||
            r.firmware != inverter_firmware_ || memcmp(r.version, write_profile_->version, sizeof r.version) ||
            (!plan.access_only && memcmp(r.unit_id, plan.unit_id, sizeof r.unit_id)))
            w = "Read the complete settings from the current inverter before writing";
        constexpr uint8_t standalone[] = {1, 1, 1, 0, 0, 0, 0};
        constexpr uint8_t broadcast[] = {0, 0, 0, 0};
        if (!w && !write_profile_->other_runtime &&
            (r.unit_info[0] != write_profile_->model || memcmp(r.unit_info + 1, standalone, sizeof standalone) ||
             (r.unit_info[8] & 0x0F) || r.unit_info[9] ||
             !memcmp(r.unit_id, broadcast, sizeof broadcast)))
            w = "Settings writes require a single standalone MultiPlus-II";
    }
    if (why) *why = w ? w : "";
    if (w) return false;
    plan_ = plan;
    // Chunk 6 of the last record reaches two bytes past the records; the Multi reads ff there.
    plan_.image[plan_.records * RECORD] = plan_.image[plan_.records * RECORD + 1] = 0xFF;
    write_ = WriteJob();
    write_.active = true;
    write_.started = now;
    write_.step = plan_.access.count ? W_AUTHORIZE : plan_.settings ? W_SETTINGS : W_MARK;
    if (plan_.access.count)
        write_.authorization_deadline = now + (plan_.access.expires() - utc + 1) * 1000000;
    note("# write: %d setting%s%s; no polls or ESS writes meanwhile\n", plan_.settings, plan_.settings == 1 ? "" : "s",
         plan_.assistants ? ", then the assistants and a reset" : "");
    return true;
}

bool Requester::write_job(int64_t now, Kind *kind) {
    WriteJob &j = write_;
    if (!j.active || (j.wait_until != NONE && now < j.wait_until)) return false;
    if (plan_.access_only && j.step != W_AUTHORIZE) {
        write_end("credentials-only guard stopped a non-authorization step");
        return false;
    }
    switch (j.step) {
    case W_AUTHORIZE:
        if (now >= j.authorization_deadline) {
            write_end("embedded authorization expired before settings were written");
            return false;
        }
        *kind = ACCESS_WRITE;
        return true;
    case W_SETTINGS: *kind = SETTING_WRITE; return true;
    case W_MARK:
    case W_UNMARK: *kind = UNIT_WRITE; return true;
    case W_RECORDS: *kind = RECORD_WRITE; return true;
    case W_RESET: *kind = RESET_SEND; return true;
    case W_RESTART:
        // The Multi should be back. Another master heard since the reset is left the start-up (the MK3
        // made it after VRM's upload); otherwise this one goes through it. The Multi's own break frames
        // don't count (multi_break_frame()).
        if (other_end_ != NONE && other_end_ > j.reset_at) {
            j.startup_skipped = true;
            if (other_intact_) {
                snprintf(j.startup, sizeof j.startup, "start-up left out: another master (%02X %02X) was heard after the reset",
                         other_addr_[0], other_addr_[1]);
            } else {
                snprintf(j.startup, sizeof j.startup, "start-up left out: a garbled frame (%02X %02X ..) was heard after the reset",
                         other_addr_[0], other_addr_[1]);
            }
            write_end(nullptr);
            return false;
        }
        j.step = W_STARTUP;
        j.index = 0;
        *kind = STARTUP_SEND;
        return true;
    case W_STARTUP: *kind = STARTUP_SEND; return true;
    default: return false;
    }
}

size_t Requester::write_payload(Kind kind, uint8_t id, uint8_t *payload) const {
    const WriteJob &j = write_;
    const WritePlan &p = plan_;
    if (p.access_only && kind != ACCESS_WRITE) return 0;
    size_t n = 0;
    switch (kind) {
    case ACCESS_WRITE:
        payload[n++] = 0x00;
        payload[n++] = id;
        payload[n++] = 0x3A;
        memcpy(payload + n, p.access.bytes[j.index], SettingsAccess::BYTES);
        n += SettingsAccess::BYTES;
        break;
    case SETTING_WRITE: {  // WriteViaID, flags 01: a setting, kept in EEPROM
        uint16_t v = p.setting_value[j.index];
        const uint8_t b[] = {0x00, id, 0x37, 0x01, p.setting_id[j.index], (uint8_t)(v & 0xFF), (uint8_t)(v >> 8)};
        memcpy(payload, b, n = sizeof b);
        break;
    }
    case UNIT_WRITE:  // 21 <ID> 04 and the unit's bytes 2-10
        payload[n++] = 0x21;
        memcpy(payload + n, p.unit_id, 4);
        n += 4;
        payload[n++] = 0x04;
        memcpy(payload + n, j.step == W_MARK ? p.unit_marked : p.unit_final, 9);
        n += 9;
        break;
    case RECORD_WRITE:  // 0x58 <record> blanks it; 0x59..0x5E <record> <11 bytes> write chunks 1..6
        payload[n++] = 0x00;
        payload[n++] = id;
        payload[n++] = (uint8_t)(0x58 + j.chunk);
        payload[n++] = (uint8_t)j.index;
        if (j.chunk) {
            memcpy(payload + n, p.image + j.index * RECORD + (j.chunk - 1) * CHUNK, CHUNK);
            n += CHUNK;
        }
        break;
    case RESET_SEND: {
        const uint8_t b[] = {0x21, 0x00, 0x00, 0x00, 0x00, 0x03};
        memcpy(payload, b, n = sizeof b);
        break;
    }
    case STARTUP_SEND: {
        const StartupFrame &f = STARTUP[j.index];
        memcpy(payload, f.bytes, n = f.n);
        if (f.to_unit) memcpy(payload + 1, p.unit_id, 4);
        break;
    }
    default: break;
    }
    return n;
}

// A reply to the write job's request, or none (p null; the frames that get none come here once their
// echo is in): on to the next, or up to three tries.
void Requester::write_reply(const uint8_t *p, size_t k) {
    WriteJob &j = write_;
    if (!j.active) return;
    bool went_out = echo_[0] == 'o' || echo_[0] == 'c';  // "ok" or "counted"
    bool ok = false;
    switch (kind_) {
    case ACCESS_WRITE:
        j.access_reply = p && k >= 3 ? p[2] : -1;
        ok = p && k >= 3 && p[2] == 0x9C;
        if (p && !ok) {
            write_end("embedded authorization was rejected; no settings were written");
            return;
        }
        break;
    case SETTING_WRITE: ok = p && k >= 3 && p[2] == 0x88; break;  // WriteSettingOK
    case RECORD_WRITE: ok = p && k >= 3 && p[2] == 0x97; break;
    case UNIT_WRITE:
    case RESET_SEND: ok = went_out; break;
    case STARTUP_SEND:
        if (STARTUP[j.index].reply) {
            j.identified = j.identified || p != nullptr;
            if (j.maintenance && p && k >= 5) memcpy(plan_.unit_id, p + 1, 4);
            if (j.maintenance && !p) {
                write_end("Redetection failed: the unit did not answer the roll call");
                return;
            }
            ok = true;  // answered or not, the MK3 went on
        } else {
            ok = went_out;
        }
        break;
    default: return;
    }
    if (ok) {
        write_advance();
        return;
    }
    if (kind_ == SETTING_WRITE && p && k >= 3 && p[2] == 0x9B) {
        char why[112];
        snprintf(why, sizeof why, "access denied for setting %u; %d of %d settings written; reopen in VEConfigure",
                 plan_.setting_id[j.index], j.settings_written, plan_.settings);
        write_end(why);
        return;
    }
    if (++j.tries < 3) return;  // the same request again
    char why[112], got[24];
    snprintf(got, sizeof got, p && k >= 3 ? "reply 0x%02X" : "no reply", p && k >= 3 ? p[2] : 0);
    switch (j.step) {
    case W_AUTHORIZE:
        snprintf(why, sizeof why, "embedded authorization was not acknowledged (%s); no settings were written", got);
        break;
    case W_SETTINGS:
        snprintf(why, sizeof why, "setting %u was not written (%s); %d of %d were, nothing else", plan_.setting_id[j.index],
                 got, j.settings_written, plan_.settings);
        break;
    case W_RECORDS:
        snprintf(why, sizeof why, "assistant record %d was not written (%s); the assistant flag stays off", j.index, got);
        break;
    default:
        snprintf(why, sizeof why, "the %s frame did not go out%s", write_step_name(j.step),
                 j.step == W_MARK || j.step == W_UNMARK ? "; the assistant flag may be off" : "");
        break;
    }
    write_end(why);
}

void Requester::write_advance() {
    WriteJob &j = write_;
    j.tries = 0;
    j.wait_until = NONE;
    switch (j.step) {
    case W_AUTHORIZE:
        erase_secret(plan_.access.bytes[j.index], SettingsAccess::BYTES);
        j.authorized++;
        if (++j.index < (int)plan_.access.count) return;
        if (plan_.access_only) {
            note("# access test: embedded authorization accepted; no settings writes or reset\n");
            write_end(nullptr);
            return;
        }
        j.index = 0;
        j.step = plan_.settings ? W_SETTINGS : W_MARK;
        note("# write: embedded authorization accepted; applying settings\n");
        return;
    case W_SETTINGS:
        note("# write: setting %u = %u (0x%04X) written\n", plan_.setting_id[j.index], plan_.setting_value[j.index],
             plan_.setting_value[j.index]);
        j.settings_written++;
        if (++j.index < plan_.settings) return;
        if (!plan_.assistants && !plan_.restart) {
            write_end(nullptr);
            return;
        }
        j.step = plan_.assistants ? W_MARK : W_RESET;
        j.index = 0;
        return;
    case W_MARK:
        j.step = W_RECORDS;
        j.index = plan_.records - 1;
        j.chunk = first_chunk(plan_, j.index);
        note("# write: assistant flag off; writing %d assistant records, the last first\n", plan_.records);
        return;
    case W_RECORDS:
        if (j.chunk > 1) {
            j.chunk--;
            return;
        }
        j.records_written++;
        if (--j.index >= 0) {
            j.chunk = first_chunk(plan_, j.index);
            return;
        }
        j.step = W_UNMARK;
        return;
    case W_UNMARK:
        j.step = W_RESET;
        j.index = 0;
        note("# write: %d assistant records written, assistant flag on; resetting the Multi\n", j.records_written);
        return;
    case W_RESET:
        if (++j.index < 2) {
            j.wait_until = now_ + 50000;  // twice, 50 ms apart, as mk2vsc
            return;
        }
        j.step = W_RESTART;
        j.reset_at = now_;
        j.wait_until = now_ + WRITE_RESTART_US;
        return;
    case W_STARTUP:
        if (STARTUP[j.index].wait_us) j.wait_until = now_ + STARTUP[j.index].wait_us;
        if (++j.index < STARTUP_FRAMES) return;
        snprintf(j.startup, sizeof j.startup, "start-up made; the unit %s the roll call",
                 j.identified ? "answered" : "did not answer");
        write_end(nullptr);
        return;
    default:
        return;
    }
}

void Requester::write_end(const char *problem) {
    plan_.access.erase_bytes();
    WriteJob &j = write_;
    j.active = false;
    j.done = true;
    j.failed = problem != nullptr;
    j.finished = now_;
    j.wait_until = NONE;
    if (problem) snprintf(j.problem, sizeof j.problem, "%s", problem);
    if (j.maintenance && operation_.active())
        operation_end(!problem && j.identified, problem ? "VE.Bus maintenance failed; check the bus and inverter"
                      : j.identified ? "VE.Bus start-up completed; the inverter answered the roll call" : "VE.Bus redetection was not confirmed");
    // The controller starts afresh: the ESS check again (the assistants may have moved), the flag word
    // read again, the ramp from the Multi's setpoint, every poll due.
    reset_ess_layout();
    snap_taken_ = false;
    for (int64_t &t : ctl_next_) t = 0;
    if (problem) {
        note("# write: stopped: %s\n", problem);
    } else {
        note("# write: done in %.1f s: %d setting%s%s%s%s\n", (j.finished - j.started) / 1e6, j.settings_written,
             j.settings_written == 1 ? "" : "s", plan_.assistants ? ", the assistants and a reset" : plan_.restart ? ", and a reset" : "",
             j.startup[0] ? "; " : "", j.startup);
    }
}

void Requester::back_off(const char *why) {
    reset_ess_layout();
    ctl_backoff_until_ = now_ + CTL_BACKOFF_US;
    ctl_.backoffs++;
    ctl_failed_in_a_row_ = ctl_unacked_in_a_row_ = ctl_unanswered_in_a_row_ = 0;
    note("# control: backing off for 30 s: %s\n", why);
}

Requester::CtlState Requester::control_state(int64_t now) const {
    if (!ctl_enabled_) return CtlState::OFF;
    if (on_) return CtlState::TESTING;
    if (first_heard_ == NONE || now - first_heard_ < BOOT_LISTEN_US) return CtlState::LISTENING;
    if (other_end_ != NONE && now - other_end_ < OTHER_MASTER_QUIET_US) return CtlState::PAUSED;
    if (ctl_backoff_until_ != NONE && now < ctl_backoff_until_) return CtlState::BACKING_OFF;
    if (ctl_armed_) return ess_setpoint_ram() && !ess_firmware_gate(now) ? CtlState::WRITING : CtlState::CHECKING;
    return CtlState::POLLING;
}

const char *Requester::control_state_name(int64_t now) const { return CTL_NAME[(int)control_state(now)]; }

const char *Requester::firmware_gate(int64_t now) const {
    if (!inverter_firmware_ || sync_t0_ < 0 || now < sync_t0_ || now - sync_t0_ >= LINK_STALE_US)
        return "Waiting for verified inverter firmware";
    return runtime_firmware_verified() ? nullptr : "Writes are not verified for this inverter firmware";
}

const char *Requester::settings_write_firmware_gate(int64_t now) const {
    if (write_profile_ && write_profile_->rvsc && sync_t0_ >= 0 && now >= sync_t0_ && now - sync_t0_ < LINK_STALE_US)
        return nullptr;
    return firmware_gate(now);
}

const char *Requester::ess_firmware_gate(int64_t now) const {
    if (!inverter_firmware_ || sync_t0_ < 0 || now < sync_t0_ || now - sync_t0_ >= LINK_STALE_US)
        return "Waiting for verified inverter firmware";
    return write_profile_ ? nullptr : "ESS writes are not verified for this inverter firmware";
}

bool Requester::ignore_ac_transfer_pending(int64_t now) const {
    return ac_input_.wanted && ignore_ac_transfer_until_ >= 0 && now < ignore_ac_transfer_until_;
}

void Requester::report_control(int64_t now) {
    static const char *const TEXT[] = {"off",
                                       "waiting for the test run to end",
                                       "listening for other masters",
                                       "paused: another master (the MK3) is on the bus",
                                       "backing off after failed requests",
                                       "polling the Multi",
                                       "checking the ESS assistant before writing",
                                       "polling, and writing the setpoint every 2.5 s"};
    CtlState s = control_state(now);
    if (s == ctl_reported_) return;
    ctl_reported_ = s;
    note("# control: %s\n", TEXT[(int)s]);
}

bool Requester::control_job(int64_t now, Kind *kind) {
    static_assert(sizeof KIND_NAME/sizeof KIND_NAME[0]==KINDS,"Every bus job needs a diagnostic name");
    CtlState s = control_state(now);
    bool owner=s==CtlState::POLLING || s==CtlState::CHECKING || s==CtlState::WRITING;
    const bool runtime_ready = !firmware_gate(now);
    const bool ess_ready = !ess_firmware_gate(now);
    const bool layout_due = ess_discovery_available(now) && !ess_discovered_ram_ && !ess_absent_ && now >= ess_retry_at_;
    acinput::Wire::Job acjob=ac_input_.job(now);
    if (acjob == acinput::Wire::WRITE && !runtime_ready) acjob = acinput::Wire::NONE;
    if (acjob == acinput::Wire::VERSION && now < next_firmware_probe_) acjob = acinput::Wire::NONE;
    auto pvjob=pv_.job(now,runtime_ready && pv_.enabled,ess_setpoint_ram() != 0);
    if(pvjob==pvdisable::Wire::WRITE && (multi_flags_<0 || multi_flags_!=pv_.raw))pvjob=pvdisable::Wire::READ;
    // Restore PV before releasing Ignore AC, so the raised island frequency
    // cannot delay reconnection. All reads/writes use this same serialized bus.
    if(owner && !pv_.wanted && pvjob!=pvdisable::Wire::NONE){*kind=pvjob==pvdisable::Wire::READ?FLAGS:PV_WRITE;return true;}
    // Safety release precedes user configuration and ordinary ESS writes.
    if(owner && acjob==acinput::Wire::WRITE && !ac_input_.wanted){*kind=AC_WRITE;return true;}
    if (write_.active) {
        // The write has the bus to itself. Another master stops it before the assistant flag is set;
        // after that the writes go on in the cycles it leaves free.
        if (s == CtlState::PAUSED && !write_marked()) {
            char why[112];
            snprintf(why, sizeof why, "another master (the MK3) started talking: stopped after %d of %d settings, "
                                      "before the assistants",
                     write_.settings_written, plan_.settings);
            write_end(why);
            return false;
        }
        if (s == CtlState::OFF || s == CtlState::BACKING_OFF) return false;
        if (write_.maintenance && !write_.reset_sends && !runtime_ready) {
            write_end(firmware_gate(now));
            return false;
        }
        return write_job(now, kind);
    }
    // A settings read asked for on the page goes on with another master on the bus too, in the
    // cycles it leaves free (due() yields the others).
    if (s == CtlState::PAUSED) return !settings_.inspection && settings_job(kind);
    if (owner && now >= next_firmware_probe_ && !charger_.active() && !settings_.active) {
        *kind = AC_VERSION;
        return true;
    }
    if (owner && layout_due &&
        ((ess_ready && ctl_armed_) || (runtime_ready && pv_.enabled))) {
        *kind = ess_layout_job();
        return true;
    }
    if (s == CtlState::WRITING && now >= ctl_next_[WRITE]) {
        *kind = WRITE;
        return true;
    }
    if (s != CtlState::POLLING && s != CtlState::CHECKING && s != CtlState::WRITING) return false;
    if(pvjob!=pvdisable::Wire::NONE){*kind=pvjob==pvdisable::Wire::READ?FLAGS:PV_WRITE;return true;}
    if(acjob!=acinput::Wire::NONE){
        *kind=acjob==acinput::Wire::VERSION?AC_VERSION:acjob==acinput::Wire::WRITE?AC_WRITE:AC_STATUS;
        return true;
    }
    if(s==CtlState::POLLING&&runtime_ready&&external_battery_enabled_&&
       (extra_current_pending_||external_current_active_)){
        // Finish an external SOC/current transaction after ESS is stopped,
        // without rearming ESS. This branch is below the same runtime/profile,
        // ownership and settings-job gates as ordinary control traffic.
        *kind=EXTRA_CURRENT_SEND;return true;
    }
    if(s==CtlState::WRITING&&runtime_ready&&external_battery_enabled_){
        // Always release the SOC estimator immediately after sending external
        // SOC; continue refreshing zero current even if its MQTT feed stops.
        if(extra_current_pending_||now>=ctl_next_[EXTRA_CURRENT_SEND]){*kind=EXTRA_CURRENT_SEND;return true;}
        if(external_soc_valid_&&now>=ctl_next_[SOC_SEND]){*kind=SOC_SEND;return true;}
    }
    if (runtime_ready && operation_.active() && now >= operation_.next_send &&
        (operation_.sends == 0 || (operation_.command.action == inverter::Action::MODE && operation_.sends < 2))) {
        *kind = OPERATE;
        return true;
    }
    // The flag word, once the check has found it and a read shows the Multi's differs.
    if (ess_ready && ess_setpoint_ram() && ctl_flags_ >= 0 && multi_flags_ >= 0 &&
        ((multi_flags_ ^ ctl_flags_) & ESS_FLAG_BITS) && now >= ctl_next_[FLAGS_WRITE]) {
        *kind = FLAGS_WRITE;
        return true;
    }
    static const Kind POLLS[] = {SNAPSHOT, DC_INFO, L1_INFO, FLAGS, CHARGE_STATUS};
    for (Kind k : POLLS) {
        if (now >= ctl_next_[k]) {
            *kind = k == SNAPSHOT && !snap_taken_ ? SNAP_TAKE : k;  // take the snapshot before reading it
            return true;
        }
    }
    if(meter_probe_.active){*kind=meter_probe_.info?METER_INFO:METER_READ;return true;}
    if(fault_report_.active()){*kind=FAULT_READ;return true;}
    if (charger_job(kind)) return true;
    if (settings_job(kind)) return true;
    // Use a spare polling cycle for read-only discovery.
    if (layout_due) {
        *kind = ess_layout_job();
        return true;
    }
    if (!monitor_alarm_status_ || settings_busy() ||
        !ac_input_.version_known || ac_input_.firmware != faults::TESTED_FIRMWARE) return false;
    if (fault_report_.capability_at < 0 && now >= ctl_next_[ALARM_CAPABILITY]) {
        *kind = ALARM_CAPABILITY;
        return true;
    }
    if (fault_report_.capability == 0x3f && now >= ctl_next_[ALARM_STATUS] &&
        (fault_report_.extended_at < 0 || now - fault_report_.extended_at >= faults::STATUS_PERIOD_US)) {
        *kind = ALARM_STATUS;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- the bus

void Requester::failed_one() {
    if (alarm_poll()) return;
    if (by_control_) {
        ctl_.failed++;
        ctl_failed_in_a_row_++;
    } else {
        stats_.failed++;
    }
}

// A frame from another master, or one that may be: the end of it, and where it came from.
void Requester::other_heard(const uint8_t *raw, size_t n, int64_t t0, bool intact) {
    other_end_ = t0 + (int64_t)n * BYTE_US;
    other_addr_[0] = n > 0 ? raw[0] : 0;
    other_addr_[1] = n > 1 ? raw[1] : 0;
    other_intact_ = intact;
    invalidate_runtime_controls("Cancelled: another master is on the VE.Bus");
    invalidate_charger("Cancelled: another master is on the VE.Bus; reload before saving again");
    fault_report_.cancel(t0);
}

bool Requester::heard(const uint8_t *raw, size_t n, int64_t t0, bool *counted) {
    if (counted) *counted = false;
    now_ = t0;
    if (first_heard_ == NONE) first_heard_ = t0;
    bool echo = false, own = false;
    if (phase_ == ECHO) {
        if (t0 <= t_start_ + ECHO_START_US) {
            // It starts with ours: our echo, whole or garbled.
            echo = true;
            own = n == frame_len_ && memcmp(raw, frame_, n) == 0;
            echo_ = own ? "ok" : "BAD";
            if (!own) {
                failed_one();
            } else if (by_control_) {
                if (!alarm_poll()) ctl_failed_in_a_row_ = 0;
            } else {
                stats_.echo_ok++;
            }
        } else {
            // Ours went by unheard; this frame is the next one. Did its sender count ours?
            int ours = frame_[3] & 0x7F;
            bool took = checksum_ok(raw, n) && (raw[3] & 0x7F) == ((ours + 1) & 0x7F);
            echo_ = took ? "counted" : "lost";
            if (!took) {
                failed_one();
            } else {
                if (by_control_) {
                    if (!alarm_poll()) ctl_failed_in_a_row_ = 0;
                } else {
                    stats_.counted++;
                }
                last_nr_ = ours;  // so the step over ours isn't taken for a gap
                if (counted) *counted = true;
            }
        }
        phase_ = REPLY;
        if (!expect_reply_) finish(NONE, nullptr, 0);
    }
    if (!checksum_ok(raw, n)) {
        clean_ = false;  // its frame number is lost, so wait for the next sync
        const bool inverter_break = multi_break_frame(raw, n);
        if (inverter_break)
            invalidate_inverter_configuration("Inverter restarted or stopped its assistants; read the settings again", true);
        if (!echo) {
            // Garbled, it may have been another master's request; not the Multi's own break frame.
            if (!from_multi(raw) && !inverter_break) other_heard(raw, n, t0, false);
            if (on_ && ++stats_.bad >= MAX_BAD) stop("bad frames on the bus");
        }
        return own;
    }
    int nr = raw[3] & 0x7F;
    if (on_ && last_nr_ >= 0 && ((nr - last_nr_) & 0x7F) != 1) stats_.gaps++;
    last_nr_ = nr;
    if (echo) return own;  // ours, whole or garbled: not another master's
    last_other_ = t0;
    if (!from_multi(raw)) {
        other_heard(raw, n, t0, true);  // another master: a request of its own, maybe
        if (on_ && mode_ == Mode::ESS) stop("another master is on the bus");
    } else if (is_sync(raw, n)) {
        if (sync_t0_ != NONE && t0 - sync_t0_ >= LINK_STALE_US)
            invalidate_inverter_configuration("VE.Bus reconnected; read the inverter settings again");
        prev_sync_t0_ = sync_t0_;
        sync_t0_ = t0;
        clean_ = true;
        if (phase_ == REPLY && syncs_since_ < 255) syncs_since_++;
    } else if (phase_ == REPLY && raw[2] == 0xFE) {
        uint8_t p[16];
        size_t k = payload_of(raw, n, p, sizeof p);
        if (kind_ == DC_INFO || kind_ == L1_INFO) {
            // an info frame answers it: no id, but which one it is
            if (k >= 6 && p[0] == 0x20 && p[5] == (kind_ == DC_INFO ? 0x0C : 0x08)) finish(t0, p, k);
        } else if (kind_ == UNIT_READ || kind_ == CHARGER_UNIT || kind_ == ESS_UNIT) {
            // so does 21 .. 05: the unit's ID and ten bytes, the first of them (0x2F here) where an info
            // frame carries its phase code, 0x05 to 0x0C
            if (k >= 15 && p[0] == 0x20 && (p[5] < 0x05 || p[5] > 0x0C)) finish(t0, p, k);
        } else if (kind_ == STARTUP_SEND) {
            // the roll call's 21 .. 00: the unit answers with frame 0x20 and its ID
            bool unit_reply = k >= 6 && p[0] == 0x20 && (p[5] < 0x05 || p[5] > 0x0C);
            if (unit_reply && (write_.maintenance || memcmp(p + 1, plan_.unit_id, 4) == 0)) finish(t0, p, k);
        } else if (k >= 3 && p[0] == 0x00 && p[1] == frame_[5]) {
            SettingsRead &r = settings_;
            if (kind_ == BLOCK_READ && p[2] == 0x9E) {
                // Winmon 0x3C answers twice, 0x9E then 0x9F in the next cycle: keep the first, wait for the second
                if (r.active && sent_settings_generation_ == settings_generation_ &&
                    r.part == BLOCK_PART && k >= 12 && p[3] == BLOCK_ARG[r.step]) {
                    memcpy(r.block[r.step], p + 4, 8);
                    r.block_got[r.step] |= 1;
                }
            } else {
                finish(t0, p, k);  // a reply carries our id back
            }
        }
    }
    return own;
}

const uint8_t *Requester::sent_frame(size_t *n, int64_t *t_start) const {
    *n = frame_len_;
    *t_start = t_start_;
    return frame_;
}

bool Requester::due(int64_t now, uint8_t *frame, size_t cap, size_t *n, int64_t *earliest_start,
                    int64_t *latest_start) {
    if (phase_ != IDLE) return false;
    Kind kind;
    bool by_control = !on_;
    if (on_ ? !test_job(now, &kind) : !control_job(now, &kind)) return false;
    uint8_t id = (uint8_t)(FIRST_ID + next_id_ % IDS);
    const uint8_t ess_ram = ess_setpoint_ram();
    int watts = by_control ? ctl_watts_ : watts_;
    // The flag word: the ESS settings' three bits, the rest as last read, as Venus writes it.
    const int flags_word = (multi_flags_ & ~ESS_FLAG_BITS) | (ctl_flags_ & ESS_FLAG_BITS);
    planned_value_ = kind == PV_WRITE ? (multi_flags_ & ~pvdisable::MASK) | (pv_.wanted?pvdisable::MASK:0)
                     : kind == AC_WRITE ? int(ac_input_.wanted) : kind == FLAGS_WRITE     ? flags_word
                     : kind >= ACCESS_WRITE ? write_.index
                     : kind >= SERIAL_READ   ? settings_.step
                                             : watts;
    const uint8_t sid = (uint8_t)(settings_.step < 0 ? 0 : settings_.step);
    uint8_t payload[16];
    size_t pn = 0;
    auto put = [&](std::initializer_list<uint8_t> bytes) {
        for (uint8_t b : bytes) payload[pn++] = b;
    };
    switch (kind) {
    case READ: put({0x00, id, 0x30, RAM_SOC, RAM_MAINS_V}); break;
    case ESS_UNIT: put({0x21, 0x00, 0x00, 0x00, 0x00, 0x05}); break;
    case CHECK: put({0x00, id, 0x30, ess_header_, 0x00}); break;
    case ESS_REGISTERS: put({0x00, id, 0x30, ess_candidate_, uint8_t(ess_candidate_ + 1)}); break;
    case WRITE:  // WriteViaID, RAM variable (flags 0x00, as Venus), the setpoint, value
        put({0x00, id, 0x37, 0x00, ess_ram, (uint8_t)(watts & 0xFF), (uint8_t)((watts >> 8) & 0xFF)});
        break;
    case SNAPSHOT: put({0x00, id, 0x38}); break;
    case METER_INFO: put({0x00,id,0x36,meter_probe_.values[meter_probe_.index].id,0x00}); break;
    case METER_READ: put({0x00,id,0x30,meter_probe_.values[meter_probe_.index].id}); break;
    case FAULT_READ:
        planned_fault_attempt_=fault_report_.attempt;
        put({0x00,id,0x44,fault_report_.selector()});break;
    case ALARM_CAPABILITY: put({0x00,id,0x44,0x80}); break;
    case ALARM_STATUS: put({0x00,id,0x44,0xff}); break;
    case DC_INFO: put({0x21, 0x00, 0x00, 0x00, 0x00, 0x08, 0xFF}); break;
    case L1_INFO: put({0x21, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00}); break;
    case FLAGS: put({0x00, id, 0x30, ess_ram ? uint8_t(ess_ram + 1) : RAM_MAINS_V, RAM_SOC}); break;
    case SOC_SEND: {
        int value=external_soc_raw_;if(value<0)value=0;if(value>200)value=200;
        planned_value_=value;put({0x38,0x04,(uint8_t)value,(uint8_t)(value>>8),0x00});break;
    }
    case EXTRA_CURRENT_SEND: {
        int value=ctl_armed_?extra_current_raw_:0;if(value<0)value=0;if(value>2000)value=2000;
        planned_value_=value;put({0x38,0x05,(uint8_t)value,(uint8_t)(value>>8),(uint8_t)(value>>16)});break;
    }
    case CHARGE_STATUS: put({0x00, id, 0x0E, 0x00, 0x00}); break;
    case AC_VERSION: put({0x00,id,0x05}); break;
    case AC_WRITE: put({0x00,id,0x37,0x00,0x17,uint8_t(ac_input_.wanted?0x08:0x00),0x00}); break;
    case AC_STATUS: put({0x00,id,0x30,0x0B,0x00}); break;
    case PV_WRITE: put({0x00,id,0x37,0x00,uint8_t(ess_ram+1),uint8_t(planned_value_),uint8_t(planned_value_>>8)});break;
    case CHARGER_VERSION: put({0x00, id, 0x05}); break;
    case CHARGER_UNIT: put({0x21, 0x00, 0x00, 0x00, 0x00, 0x05}); break;
    case CHARGER_INFO: put({0x00, id, 0x35, charger::setting_id(charger_index_, charger_.profile), 0x00}); break;
    case CHARGER_READ: put({0x00, id, 0x31, charger::setting_id(charger_index_, charger_.profile), 0x00}); break;
    case CHARGER_WRITE: {
        uint16_t value = charger_change_.raw[charger_index_];
        put({0x00, id, 0x37, 0x01, charger::setting_id(charger_index_, charger_.profile), (uint8_t)value, (uint8_t)(value >> 8)});
        break;
    }
    case OPERATE: {
        const inverter::Command &c = operation_.command;
        if (c.action == inverter::Action::MODE) put({0x3F, (uint8_t)c.value, 0x00, 0x00, 0x00});
        else if (c.action == inverter::Action::CURRENT) put({0x3F, 0xC0, 0x71, (uint8_t)c.value, (uint8_t)(c.value >> 8)});
        else {
            uint8_t state = c.action == inverter::Action::EQUALIZE ? 1 : c.action == inverter::Action::ABSORPTION ? 2 : 3;
            put({0x00, id, 0x0E, state, 0x00});
        }
        break;
    }
    case SNAP_TAKE: put({0x21, 0x00, 0x00, 0x00, 0x00, 0x0E, 0x0E, 0x10, 0x0F, 0x1A}); break;  // no reply
    case SERIAL_READ: put({0x00, id, 0x3B}); break;
    case VERSION_READ: put({0x00, id, 0x05}); break;  // SoftwareVersionPart0
    case INFO_READ: put({0x00, id, 0x35, setting_id(settings_), 0x00}); break;     // GetSettingInfo
    case SETTING_READ: put({0x00, id, 0x31, setting_id(settings_), 0x00}); break;  // ReadSetting
    case UNIT_READ: put({0x21, 0x00, 0x00, 0x00, 0x00, 0x05}); break;  // the unit's ID and model bytes
    case BLOCK_READ: put({0x00, id, 0x3C, BLOCK_ARG[sid % BLOCK_ARGS], 0x00}); break;
    case GRID_READ: put({0x00, id, 0x57, settings_.chunk, sid}); break;       // chunk, record
    case ASSISTANT_READ: put({0x00, id, 0x5F, settings_.chunk, sid}); break;  // chunk, record
    case ACCESS_WRITE:
    case SETTING_WRITE:
    case UNIT_WRITE:
    case RECORD_WRITE:
    case RESET_SEND:
    case STARTUP_SEND: pn = write_payload(kind, id, payload); break;
    case FLAGS_WRITE:  // the flag word follows the setpoint
        put({0x00, id, 0x37, 0x00, (uint8_t)(ess_ram + 1), (uint8_t)(flags_word & 0xFF),
             (uint8_t)((flags_word >> 8) & 0xFF)});
        break;
    default: return false;
    }
    size_t k = build_frame(0x98, 0xF7, 0xFE, (uint8_t)(last_nr_ + 1), payload, pn, frame, cap);
    if (k == 0) return false;
    int64_t frame_us = (int64_t)k * BYTE_US + ADDRESS_PAUSE_US;
    if (on_ && mode_ == Mode::BENCH) {
        if (last_other_ != NONE && now - last_other_ < BENCH_QUIET_US) return false;  // that's a live bus
        *earliest_start = now;
        *latest_start = now + 5000;
    } else {
        if (sync_t0_ == NONE || !clean_ || last_nr_ < 0) return false;
        *earliest_start = sync_t0_ + TARGET_END_US - frame_us;
        *latest_start = sync_t0_ + WINDOW_END_US - frame_us;
        if (now >= *latest_start) return false;
        // This cycle's reply goes to the first request after the previous cycle's cutoff. Without the
        // previous sync there's no telling whose that is.
        int64_t cycle = sync_t0_ - prev_sync_t0_;
        if (prev_sync_t0_ == NONE || cycle < 15000 || cycle > 25000) return false;
        if (other_end_ > prev_sync_t0_ + CLAIM_US) {
            if (yielded_sync_ != sync_t0_) {
                yielded_sync_ = sync_t0_;
                if (by_control) {
                    ctl_.yielded++;
                } else {
                    stats_.yielded++;
                }
            }
            return false;
        }
    }
    planned_kind_ = kind;
    planned_charger_generation_ = charger_generation_;
    planned_meter_generation_ = meter_generation_;
    planned_settings_generation_ = settings_generation_;
    planned_by_control_ = by_control;
    *n = k;
    return true;
}

void Requester::sent(const uint8_t *frame, size_t n, int64_t t_start, int64_t t_end) {
    if (n > sizeof frame_) n = sizeof frame_;
    memcpy(frame_, frame, n);
    frame_len_ = n;
    t_start_ = t_start;
    t_end_ = t_end;
    kind_ = planned_kind_;
    sent_control_generation_ = control_generation_;
    sent_charger_generation_ = planned_charger_generation_;
    sent_meter_generation_ = planned_meter_generation_;
    sent_settings_generation_ = planned_settings_generation_;
    sent_fault_attempt_ = planned_fault_attempt_;
    by_control_ = planned_by_control_;
    value_ = planned_value_;
    at_sync_ = on_ && mode_ == Mode::BENCH ? NONE : sync_t0_;
    // The snapshot trigger, the unit write, the reset and the start-up's frames get no reply, except
    // the roll call's 21 .. 00.
    bool no_reply = kind_ == SNAP_TAKE || kind_ == UNIT_WRITE || kind_ == RESET_SEND || kind_==SOC_SEND || kind_==EXTRA_CURRENT_SEND ||
                    (kind_ == OPERATE && (operation_.command.action == inverter::Action::MODE || operation_.command.action == inverter::Action::CURRENT)) ||
                    (kind_ == STARTUP_SEND && !STARTUP[write_.index].reply);
    expect_reply_ = !(on_ && mode_ == Mode::BENCH) && !no_reply;
    syncs_since_ = 0;
    echo_ = "lost";
    phase_ = ECHO;
    next_id_++;
    if (kind_ == WRITE) {
        last_write_ = t_start;
    }
    if (kind_ == AC_VERSION) next_firmware_probe_ = t_start + LINK_STALE_US;
    if (by_control_) {
        if (write_.active) {
            if (kind_ == ACCESS_WRITE) write_.access_requests++;
            if (kind_ == SETTING_WRITE || kind_ == UNIT_WRITE || kind_ == RECORD_WRITE) write_.config_sends++;
            if (kind_ == RESET_SEND) write_.reset_sends++;
        }
        static const int64_t PERIOD[KINDS] = {PERIOD_US, PERIOD_US, WRITE_PERIOD_US, SNAPSHOT_PERIOD_US,
                                              INFO_PERIOD_US, INFO_PERIOD_US, FLAGS_PERIOD_US, 0,
                                              WRITE_PERIOD_US, INFO_PERIOD_US}; // remaining jobs: no delay
        if(kind_==AC_VERSION || kind_==AC_WRITE || kind_==AC_STATUS)
            ac_input_.sent(kind_==AC_VERSION?acinput::Wire::VERSION:kind_==AC_WRITE?acinput::Wire::WRITE:acinput::Wire::STATUS,t_start,value_);
        if(kind_==FLAGS && pv_.enabled)pv_.reading(t_start);
        if(kind_==PV_WRITE){pv_.sent(!!(value_&pvdisable::MASK),t_start);multi_flags_=-1;}
        if (kind_ == OPERATE) {
            operation_.sent_at = t_start;
            operation_.sends++;
            operation_.progress = inverter::Progress::SENDING;
        }
        if (kind_ == CHARGER_WRITE) {
            charger_.writes++;
            charger_.parameter[charger_index_].known = false; // outcome unknown until read back
            settings_.done = false; // a complete backup needs a fresh full read after configuration changes
        }
        if (kind_ == RESET_SEND) invalidate_inverter_configuration("Inverter restarted; read the settings again");
        if(kind_==FAULT_READ)fault_report_.requests++;
        if (write_.active && write_.maintenance && (kind_ == RESET_SEND || kind_ == STARTUP_SEND)) {
            operation_.sent_at = t_start;
            operation_.sends++;
            operation_.progress = inverter::Progress::SENDING;
            snprintf(operation_.message, sizeof operation_.message, "%s",
                     kind_ == RESET_SEND ? "Restarting the VE.Bus system" : "Redetecting the VE.Bus system");
        }
        if (kind_ == SNAP_TAKE) {
            ctl_next_[SNAPSHOT] = t_start + 100000;  // trigger again unless it went through (finish_control)
        } else if (alarm_poll()) {
            ctl_next_[kind_] = t_start + (kind_ == ALARM_STATUS ? faults::STATUS_PERIOD_US : faults::COOLDOWN_US);
        } else {
            ctl_next_[kind_] = t_start + PERIOD[kind_];
            if (kind_ == SNAPSHOT) snap_taken_ = false;
        }
        if(kind_==SOC_SEND){
            ctl_next_[SOC_SEND]=t_start+10000000;extra_current_pending_=true;
            soc_sync_.soc_sent++;soc_sync_.last_soc=t_start;soc_sync_.target_soc=value_*0.5f;
        }else if(kind_==EXTRA_CURRENT_SEND){
            ctl_next_[EXTRA_CURRENT_SEND]=t_start+3000000;extra_current_pending_=false;
            external_current_active_=value_!=0;
            soc_sync_.current_sent++;soc_sync_.last_current=t_start;
        }
        if (kind_ == WRITE) {
            ctl_.writes++;
            ctl_last_write_ = t_start;
            ctl_written_w_ = value_;
        } else if (kind_ == FLAGS_WRITE || kind_==PV_WRITE) {
            ctl_.flag_writes++;
        } else {
            ctl_.polls++;
        }
    } else {
        stats_.sent++;
        const bool probing_ess = kind_ == ESS_UNIT || kind_ == CHECK || kind_ == ESS_REGISTERS;
        next_due_ = t_start + (kind_ == WRITE ? WRITE_PERIOD_US : probing_ess ? 0 : PERIOD_US);
        if (kind_ == WRITE) stats_.writes++;
    }
}

void Requester::gave_up(bool late) {
    if (planned_by_control_) return;  // the controller just tries again in a later cycle
    if (late) {
        stats_.late++;
    } else {
        stats_.busy++;
    }
}

void Requester::tick(int64_t now) {
    now_ = now;
    if (phase_ == ECHO && now - t_end_ > ECHO_TIMEOUT_US) {  // nothing at all heard since ours
        failed_one();
        phase_ = REPLY;
        if (!expect_reply_) finish(NONE, nullptr, 0);
    }
    if (phase_ == REPLY && now - t_end_ > REPLY_TIMEOUT_US) finish(NONE, nullptr, 0);
    if (on_ && now >= until_ && phase_ == IDLE) stop("time is up");
    if (settings_.inspection && settings_.active) {
        if (!inspection_link_ready(now))
            settings_finish("Connection lost; read the inverter settings again");
        else if (now - settings_.started >= INSPECTION_TIMEOUT_US)
            settings_finish("The commissioning read timed out");
    }
    if(fault_report_.active()){
        CtlState s=control_state(now);
        if((s!=CtlState::POLLING&&s!=CtlState::CHECKING&&s!=CtlState::WRITING)||now-fault_report_.started>faults::TIMEOUT_US)
            fault_report_.cancel(now);
    }
    if(meter_probe_.active){
        CtlState s=control_state(now);
        if((s!=CtlState::POLLING&&s!=CtlState::CHECKING&&s!=CtlState::WRITING)||now-meter_probe_.started>20000000){
            meter_probe_.active=false;meter_generation_++;note("# meters: cancelled; bus control unavailable or probe timed out\n");
        }
    }
    if (charger_.active()) {
        CtlState s = control_state(now);
        if (s != CtlState::POLLING && s != CtlState::CHECKING && s != CtlState::WRITING)
            charger_end(false, "Charger configuration cancelled: S3 bus control is unavailable");
        else if (now - charger_.started > 15000000)
            invalidate_charger("Charger configuration timed out; reload to check which values were stored", false);
    }
    if (operation_.active()) {
        CtlState s = control_state(now);
        if (s == CtlState::OFF || s == CtlState::TESTING || s == CtlState::PAUSED || s == CtlState::BACKING_OFF) {
            if (write_.active && write_.maintenance) write_end("Maintenance cancelled: S3 bus control is unavailable");
            else operation_end(false, "Cancelled: S3 bus control is unavailable");
        } else if (now - operation_.started > (write_.active && write_.maintenance ? 30000000 : 15000000)) {
            if (write_.active && write_.maintenance) write_end("Maintenance timed out");
            else operation_end(false, "Change was not confirmed; check the physical switch, remote panel and inverter state");
        } else if (operation_.sends == 0 && !write_.active && now - operation_.started > 3000000) {
            operation_end(false, "Command expired before it could be sent");
        }
    }
    report_control(now);
}

// One line per test request: when it went out, its echo, and the reply or its absence. Then the
// checks that switch a test run off, so their summary comes after the line of the request that
// tripped them. The controller's requests are handled in finish_control().
void Requester::finish(int64_t t_reply, const uint8_t *p, size_t k) {
    const bool probing_ess = kind_ == ESS_UNIT || kind_ == CHECK || kind_ == ESS_REGISTERS;
    if (kind_ >= SERIAL_READ && kind_ <= ASSISTANT_READ) {
        if (sent_settings_generation_ != settings_generation_ || !settings_.active) {
            phase_ = IDLE;
            return;
        }
        if (settings_.inspection && p && (t_reply < t_end_ || t_reply - t_end_ > REPLY_TIMEOUT_US)) {
            p = nullptr;
            k = 0;
        }
    }
    if (sent_control_generation_ != control_generation_ &&
        (probing_ess || kind_ == AC_VERSION || kind_ == FLAGS || kind_ == OPERATE ||
         kind_ == AC_WRITE || kind_ == AC_STATUS || kind_ == PV_WRITE || kind_ == WRITE || kind_ == FLAGS_WRITE)) {
        phase_ = IDLE;
        return;
    }
    if (probing_ess) {
        const bool fresh_reply = p && t_reply >= t_end_ && t_reply - t_end_ <= REPLY_TIMEOUT_US;
        if (const char *problem = ess_layout_reply(fresh_reply ? p : nullptr, k)) {
            reset_ess_layout();
            ess_retry_at_ = now_ + ESS_LAYOUT_RETRY_US;
            note("# ess: %s\n", problem);
            if (!by_control_) stop_why_ = problem;
        }
        if (!by_control_ && ess_absent_) stop_why_ = "No ESS Assistant found in the live layout";
        if (by_control_) {
            if (fresh_reply) {
                ctl_.answered++;
                ctl_unanswered_in_a_row_ = 0;
            } else {
                ctl_.unanswered++;
            }
            phase_ = IDLE;
            return;
        }
    }
    char head[112];
    if (at_sync_ != NONE) {
        snprintf(head, sizeof head, "%s id=%02X nr=%02X start=+%.3f end=+%.3f ms after the sync", KIND_NAME[kind_],
                 frame_[5], frame_[3], (t_start_ - at_sync_) / 1000.0, (t_end_ - at_sync_) / 1000.0);
    } else {
        snprintf(head, sizeof head, "%s id=%02X nr=%02X", KIND_NAME[kind_], frame_[5], frame_[3]);
    }
    if (by_control_) {
        finish_control(t_reply, p, k, head);
        phase_ = IDLE;
        return;
    }
    if (!expect_reply_) {
        note("# tx %lld bench %s bytes=%u echo=%s\n", (long long)t_start_, head, (unsigned)frame_len_, echo_);
    } else if (p) {
        stats_.answered++;
        bool same = syncs_since_ == 0;
        if (same) stats_.same_cycle++;
        unanswered_in_a_row_ = 0;
        stats_.last_reply_ms = (t_reply - t_end_) / 1000.0f;
        stats_.last_code = p[2];
        stats_.last_values[0] = k >= 5 ? (int)u16(p, 3) : -1;
        stats_.last_values[1] = k >= 7 ? (int)u16(p, 5) : -1;
        char what[112] = "";
        if (kind_ == AC_VERSION) {
            if (k >= 7 && p[2] == 0x82) {
                observe_firmware(p, k);
                if (write_profile_) next_due_ = 0;
            } else {
                const char *message = "Inverter firmware could not be verified for test writes";
                invalidate_runtime_controls(message);
                invalidate_charger(message);
            }
            if (!write_profile_) stop_why_ = "Inverter firmware could not be verified for test writes";
        } else if (kind_ == READ && p[2] == 0x85 && k >= 7) {  // RamReadOK: RAM 13 in 0.5 %, RAM 0 in 0.01 V
            snprintf(what, sizeof what, " (SOC %.1f %%, mains %.2f V)", u16(p, 3) * 0.5, u16(p, 5) / 100.0);
        } else if (kind_ == WRITE) {
            if (p[2] == 0x87) {  // WriteRamOK
                stats_.acked++;
                unacked_in_a_row_ = 0;
            } else {
                stop_why_ = "the Multi refused the setpoint write";
            }
            snprintf(what, sizeof what, " setpoint=%+d W", watts_);
        }
        note("# tx %lld %s echo=%s reply=%02X after %.2f ms (%s) values=%d,%d%s\n", (long long)t_start_, head, echo_,
             p[2], stats_.last_reply_ms, same ? "same cycle" : "a later cycle", stats_.last_values[0],
             stats_.last_values[1], what);
    } else {
        stats_.unanswered++;
        unanswered_in_a_row_++;
        if (kind_ == AC_VERSION) {
            const char *message = "Inverter firmware could not be verified for test writes";
            invalidate_runtime_controls(message);
            invalidate_charger(message);
        }
        if (kind_ == WRITE) unacked_in_a_row_++;
        note("# tx %lld %s echo=%s reply=none\n", (long long)t_start_, head, echo_);
    }
    phase_ = IDLE;
    if (stop_why_) {
        const char *why = stop_why_;
        stop_why_ = nullptr;
        stop(why);
    } else if (stats_.failed >= MAX_FAILED) {
        stop("requests failed: heard garbled, or neither heard nor counted");
    } else if (unacked_in_a_row_ >= MAX_UNACKED_WRITES) {
        stop("setpoint writes went unacknowledged");
    } else if (unanswered_in_a_row_ >= MAX_UNANSWERED_IN_A_ROW) {
        stop("requests went unanswered");
    }
}

// The controller's requests: polls are answered silently (the decoder reads the replies); the check
// read, each write and every problem leave a line. Too many failures make it back off.
void Requester::finish_control(int64_t t_reply, const uint8_t *p, size_t k, const char *head) {
    if(kind_==PV_WRITE){
        if(p){ctl_.answered++;ctl_unanswered_in_a_row_=0;}else ctl_.unanswered++;
        pv_.reply(p,k,now_);if(pv_.ack)ctl_.flag_acks++;
        multi_flags_=-1;ctl_next_[FLAGS]=now_+250000;
        note("# PV disable: word=0x%04X ack=%s; actual RAM readback pending\n",value_,pv_.ack?"yes":"no");
    }else if(kind_==AC_VERSION || kind_==AC_WRITE || kind_==AC_STATUS){
        if(p){ctl_.answered++;ctl_unanswered_in_a_row_=0;}else ctl_.unanswered++;
        if (kind_ == AC_VERSION) {
            if (p && k >= 7 && p[2] == 0x82) {
                observe_firmware(p, k);
            } else if (inverter_firmware_ || ignore_ac_transfer_pending(now_)) {
                const char *message = "Inverter firmware could not be verified; fresh device checks required";
                invalidate_runtime_controls(message);
                invalidate_charger(message);
                next_firmware_probe_ = now_ + LINK_STALE_US;
            } else {
                ac_input_.reply(acinput::Wire::VERSION, p, k, now_);
            }
        } else {
            ac_input_.reply(kind_ == AC_WRITE ? acinput::Wire::WRITE : acinput::Wire::STATUS, p, k, now_);
        }
        if(kind_==AC_WRITE)note("# ac input: value=%d ack=%s; actual readback pending\n",value_?8:0,ac_input_.ack?"yes":"no");
    }else if(kind_==SOC_SEND||kind_==EXTRA_CURRENT_SEND){
        // Broadcasts have no ACK. Acceptance is verified from later inverter
        // RAM/SOC replies; observing our own transmitted value is insufficient.
        if(echo_[0]!='o'&&echo_[0]!='c'){
            ctl_next_[kind_]=now_+1000000;
            if(kind_==EXTRA_CURRENT_SEND)extra_current_pending_=true;
        }
    }else if(alarm_poll()){
        const bool ok = kind_ == ALARM_CAPABILITY ? fault_report_.read_capability(p,k,now_) :
                                                    fault_report_.read_extended(p,k,now_);
        if (ok) {
            ctl_.answered++;
            fault_report_.firmware = faults::TESTED_FIRMWARE;
        } else {
            ctl_.unanswered++;
        }
    }else if(kind_==FAULT_READ){
        if(p){ctl_.answered++;ctl_unanswered_in_a_row_=0;}else ctl_.unanswered++;
        // A missing optional diagnostic does not trip ESS's failure backoff.
        if(sent_fault_attempt_==fault_report_.attempt)fault_report_.reply(p,k,now_);
    }else if(kind_==METER_INFO||kind_==METER_READ){
        if(p){ctl_.answered++;ctl_unanswered_in_a_row_=0;}
        // Unsupported diagnostics never disable otherwise healthy ESS.
        meter_reply(p,k);
    } else if (kind_ >= CHARGER_VERSION && kind_ <= CHARGER_WRITE) {
        if (p) { ctl_.answered++; ctl_unanswered_in_a_row_ = 0; }
        else { ctl_.unanswered++; ctl_unanswered_in_a_row_++; }
        charger_reply(p, k);
    } else if (kind_ == OPERATE) {
        if (!operation_.active()) return; // cancelled while in flight
        bool delivered = echo_[0] == 'o' || echo_[0] == 'c';
        if (!delivered || (expect_reply_ && (!p || k < 5 || p[2] != 0x94))) {
            operation_end(false, "Inverter command failed or was refused; it will not be retried");
        } else {
            operation_.progress = inverter::Progress::VERIFYING;
            operation_.next_send = t_start_ + 1000000;
            ctl_next_[CHARGE_STATUS] = now_ + 250000;
            snprintf(operation_.message, sizeof operation_.message, "Sent; waiting for inverter readback");
        }
    } else if (kind_ == CHARGE_STATUS) {
        // Unsupported charge-state enquiries must not disable otherwise healthy ESS control.
        if (p && k >= 5 && p[2] == 0x94) { ctl_.answered++; ctl_unanswered_in_a_row_ = 0; }
        else ctl_next_[CHARGE_STATUS] = now_ + 30000000;
    } else if (kind_ == SNAP_TAKE) {  // no reply to wait for: read the snapshot SNAP_READ_US after the trigger
        if (echo_[0] == 'o' || echo_[0] == 'c') {  // "ok" or "counted": it went out
            snap_taken_ = true;
            ctl_next_[SNAPSHOT] = t_start_ + SNAP_READ_US;
        }
    } else if (kind_ >= ACCESS_WRITE) {  // the write job's
        if (p) {
            ctl_.answered++;
            ctl_unanswered_in_a_row_ = 0;
        } else if (expect_reply_) {
            ctl_.unanswered++;
            ctl_unanswered_in_a_row_++;
        }
        write_reply(p, k);
    } else if (p) {
        ctl_.answered++;
        ctl_unanswered_in_a_row_ = 0;
        const char *refused = nullptr;
        if (kind_ == FLAGS) {
            multi_flags_ = ess_setpoint_ram() && p[2] == 0x85 && k >= 7 ? int(u16(p, 3)) : -1;
            if (multi_flags_ >= 0 && runtime_firmware_verified()) pv_.observe(multi_flags_, now_);
        } else if (kind_ == WRITE || kind_ == FLAGS_WRITE) {
            bool flags = kind_ == FLAGS_WRITE;
            if (flags) {
                note("# tx %lld %s echo=%s reply=%02X after %.2f ms flags=0x%02X\n", (long long)t_start_, head, echo_,
                     p[2], (t_reply - t_end_) / 1000.0, value_);
            } else {
                note("# tx %lld %s echo=%s reply=%02X after %.2f ms setpoint=%+d W\n", (long long)t_start_, head,
                     echo_, p[2], (t_reply - t_end_) / 1000.0, value_);
            }
            if (p[2] != 0x87) {  // not WriteRamOK
                refused = flags ? "the Multi refused the flag word write" : "the Multi refused the setpoint write";
            } else if (flags) {
                ctl_.flag_acks++;
                multi_flags_ = -1;                   // read it back before writing it again
                ctl_next_[FLAGS] = t_reply + 50000;  // soon
            } else {
                ctl_.acked++;
                ctl_unacked_in_a_row_ = 0;
            }
        } else if (kind_ >= SERIAL_READ) {
            settings_reply(p, k);
        }
        if (refused) back_off(refused);
    } else {
        ctl_.unanswered++;
        ctl_unanswered_in_a_row_++;
        if (kind_ == FLAGS) {
            multi_flags_ = -1;
        } else if (kind_ == WRITE) {
            ctl_unacked_in_a_row_++;
            note("# tx %lld %s echo=%s reply=none setpoint=%+d W\n", (long long)t_start_, head, echo_, value_);
        } else if (kind_ == FLAGS_WRITE) {
            note("# tx %lld %s echo=%s reply=none flags=0x%02X\n", (long long)t_start_, head, echo_, value_);
        } else if (kind_ >= SERIAL_READ) {
            settings_reply(nullptr, 0);
        }
    }
    const char *why = ctl_failed_in_a_row_ >= MAX_FAILED                     ? "3 requests failed"
                      : ctl_unacked_in_a_row_ >= MAX_UNACKED_WRITES          ? "3 setpoint writes unacknowledged"
                      : ctl_unanswered_in_a_row_ >= MAX_UNANSWERED_IN_A_ROW ? "5 requests unanswered"
                                                                             : nullptr;
    if (why) back_off(why);
}

size_t Requester::describe(int64_t now, char *out, size_t cap) const {
    const TxStats &s = stats_;
    int n;
    if (on_ && mode_ == Mode::ESS) {
        n = snprintf(out, cap, "tx=ess(%+dW,%llds) writes=%lu acked=%lu counted=%lu failed=%lu yielded=%lu",
                     watts_, (long long)((until_ - now) / 1000000), (unsigned long)s.writes, (unsigned long)s.acked,
                     (unsigned long)s.counted, (unsigned long)s.failed, (unsigned long)s.yielded);
    } else if (on_) {
        n = snprintf(out, cap,
                     "tx=%s(%llds) sent=%lu echo_ok=%lu counted=%lu failed=%lu answered=%lu unanswered=%lu yielded=%lu",
                     mode_name(mode_), (long long)((until_ - now) / 1000000), (unsigned long)s.sent,
                     (unsigned long)s.echo_ok, (unsigned long)s.counted, (unsigned long)s.failed,
                     (unsigned long)s.answered, (unsigned long)s.unanswered, (unsigned long)s.yielded);
    } else {
        n = snprintf(out, cap, "tx=off");
    }
    if (n < 0 || (size_t)n >= cap) return n > 0 ? cap - 1 : 0;
    const CtlStats &c = ctl_;
    CtlState st = control_state(now);
    char sp[16] = "";
    if (ctl_armed_) snprintf(sp, sizeof sp, ",%+dW", ctl_watts_);
    int m = snprintf(out + n, cap - n,
                     " ctl=%s%s polls=%lu answered=%lu failed=%lu writes=%lu acked=%lu flags=%d/%d",
                     CTL_NAME[(int)st], sp, (unsigned long)c.polls, (unsigned long)c.answered, (unsigned long)c.failed,
                     (unsigned long)c.writes, (unsigned long)c.acked, multi_flags_, ctl_flags_);
    if (m > 0) n += m;
    if (write_.active && (size_t)n < cap) {
        m = snprintf(out + n, cap - n, " write=%s:%d", write_step_name(write_.step), write_.index);
        if (m > 0) n += m;
    }
    return (size_t)n < cap ? (size_t)n : cap - 1;
}

size_t Requester::to_json(int64_t now, char *out, size_t cap) const {
    const TxStats &s = stats_;
    const CtlStats &c = ctl_;
    const unsigned discovered_ram = ess_discovered_ram(now);
    const unsigned control_ram = discovered_ram ? ess_setpoint_ram() : 0;
    const char *layout = !ess_discovery_available(now) ? "unavailable" : discovered_ram ? "found" :
                         ess_absent_ ? "absent" : now < ess_retry_at_ ? "retrying" : "pending";
    int n = snprintf(out, cap,
                     "\"tx\":{\"mode\":\"%s\",\"left_s\":%.0f,\"watts\":%d,\"sent\":%lu,\"echo_ok\":%lu,\"counted\":%lu,"
                     "\"failed\":%lu,\"answered\":%lu,\"same_cycle\":%lu,\"unanswered\":%lu,\"writes\":%lu,\"acked\":%lu,"
                     "\"yielded\":%lu,\"busy\":%lu,\"late\":%lu,\"bad\":%lu,\"gaps\":%lu,\"last_reply_ms\":%.2f,"
                     "\"last_code\":%d,\"last_values\":[%d,%d],\"ess_live\":null,\"why\":\"%s\"},"
                     "\"ctl\":{\"state\":\"%s\",\"armed\":%s,\"watts\":%d,\"checked\":%s,\"ess_setpoint_ram\":%u,"
                     "\"ess_layout\":\"%s\",\"ess_discovered_ram\":%u,\"flags\":%d,"
                     "\"multi_flags\":%d,\"polls\":%lu,\"answered\":%lu,\"unanswered\":%lu,\"failed\":%lu,\"writes\":%lu,"
                     "\"acked\":%lu,\"flag_writes\":%lu,\"flag_acks\":%lu,\"yielded\":%lu,\"backoffs\":%lu,"
                     "\"last_write_age_s\":%.1f}",
                     on_ ? mode_name(mode_) : "off", on_ ? (until_ - now) / 1e6 : 0.0, watts_, (unsigned long)s.sent,
                     (unsigned long)s.echo_ok, (unsigned long)s.counted, (unsigned long)s.failed,
                     (unsigned long)s.answered, (unsigned long)s.same_cycle, (unsigned long)s.unanswered,
                     (unsigned long)s.writes, (unsigned long)s.acked, (unsigned long)s.yielded, (unsigned long)s.busy,
                     (unsigned long)s.late, (unsigned long)s.bad, (unsigned long)s.gaps, s.last_reply_ms, s.last_code,
                     s.last_values[0], s.last_values[1], why_, CTL_NAME[(int)control_state(now)],
                     ctl_armed_ ? "true" : "false", ctl_watts_, control_ram ? "true" : "false",
                     control_ram, layout, discovered_ram, ctl_flags_, multi_flags_,
                     (unsigned long)c.polls, (unsigned long)c.answered, (unsigned long)c.unanswered,
                     (unsigned long)c.failed, (unsigned long)c.writes, (unsigned long)c.acked,
                     (unsigned long)c.flag_writes, (unsigned long)c.flag_acks, (unsigned long)c.yielded,
                     (unsigned long)c.backoffs, ctl_last_write_ != NONE ? (now - ctl_last_write_) / 1e6 : -1.0);
    if (n < 0 || (size_t)n >= cap) return n > 0 ? cap - 1 : 0;
    const SettingsRead &r = settings_;
    int m = snprintf(out + n, cap - n,
                     ",\"settings_read\":{\"active\":%s,\"done\":%s,\"part\":\"%s\",\"step\":%d,\"progress\":%d,"
                     "\"read\":%d,\"unsupported\":%d,\"failed\":%d}",
                     r.active ? "true" : "false", r.done ? "true" : "false", part_name(r.part), r.step, progress(r),
                     r.read, r.unsupported, r.failed);
    if (m > 0) n += m;
    if ((size_t)n >= cap) return cap - 1;
    const WriteJob &j = write_;
    m = snprintf(out + n, cap - n,
                 ",\"write\":{\"active\":%s,\"done\":%s,\"failed\":%s,\"step\":\"%s\",\"progress\":%d,\"settings_written\":%d,"
                 "\"records_written\":%d}",
                 j.active ? "true" : "false", j.done ? "true" : "false", j.failed ? "true" : "false",
                 write_step_name(j.step), write_progress(j, plan_), j.settings_written, j.records_written);
    if (m > 0) n += m;
    return (size_t)n < cap ? (size_t)n : cap - 1;
}

}  // namespace vebus
