// VE.Bus listener for the ESP32-S3. An RS-485 transceiver on UART1 (RX = GPIO18, 250,000 baud 8N1)
// hears the bus, and every frame goes out on UART0 (the board's COM socket, 921,600 baud 8N1) as one
// text line:
//   <t_us> <hex bytes>
// t_us is the estimated start of the frame's first byte, in microseconds since boot. Frames end at
// 0xFF, which never occurs inside a frame because 0xFA..0xFF are escaped. Lines starting with '#'
// carry status; they are also copied to the native USB port, whose serial output proved unreliable
// for the frames.
// It is also a bus master of its own (requester.h). Whenever no other master (an MK3) has been heard
// for 10 s it polls the Multi as Venus does, so the dashboard keeps its values. With "Run ESS from
// this ESP32" set on the ESS settings page (web.h), it runs ESS as Venus would: twice a second the
// ESS settings (ess_policy.h), kept in flash, turn the time (NTP), the SOC and the Multi's AC-out
// power into the setpoint, written every 2.5 s, and the flag word. Lines on UART0:
//   ctl arm <watts>          run ESS from here with the grid setpoint at <watts>, + = export
//   ctl disarm               stop running ESS from here
//   ctl on | ctl off         polling on (the default at boot) or off
// and test runs, during which the controller waits:
//   tx <seconds>             read requests on the live bus, at most one a second, for up to 600 s
//   bench <seconds>          the same without a Multi, to test the transceiver off the bus
//   ess <watts> <seconds>    timed setpoint writes, -100..+100 W, only with no other master
//   tx off                   (or ess off) stop a test run
// They leave on UART1's TX (GPIO17), and the transceiver's driver is switched on through its DE pin
// (GPIO16) for exactly the length of the frame, once the bus has been quiet for QUIET_US, timed to end
// just before the cycle's request cutoff. With a receive-only board, which has nothing on GPIO16 and
// GPIO17, it cannot drive the bus.
// The frames are also decoded (vebus_state.h) and, with a Wi-Fi password in wifi_secrets.h, shown
// on a dashboard at http://brain-zero.local/ (web.h) and, with a broker set on its MQTT page,
// published to Home Assistant over MQTT (mqtt_link.h).
// Build with -DVEBUS_INVERT_RX=1 if A and B are swapped (every byte then shows a framing error); TX
// is inverted with it.
#include <Arduino.h>
#include <Preferences.h>
#include <atomic>
#include <math.h>
#include <memory>
#include <new>
#include <time.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "controller_time.h"
#include "diagnostics.h"
#include "ess_policy.h"
#include "ess_lease.h"
#include "external_battery.h"
#include "firmware_ota.h"
#include "ha_mqtt.h"
#include "hal/uart_ll.h"
#include "mqtt_link.h"
#include "multiplus_ii.h"
#include "requester.h"
#include "rvsc.h"
#include "soc/gpio_struct.h"
#include "upload.h"
#include "vebus_state.h"
#include "warning_history.h"
#include "web.h"
#include "wifi_recovery.h"

#ifndef VEBUS_INVERT_RX
#define VEBUS_INVERT_RX 0
#endif
#ifndef VEBUS_CONTROL_AT_BOOT  // 0 keeps the controller off until "ctl on"
#define VEBUS_CONTROL_AT_BOOT 1
#endif

void listener_ess(web::EssView &v);

namespace {

constexpr uart_port_t BUS = UART_NUM_1;
constexpr int RX_PIN = 18;                 // from the transceiver's RO
constexpr int TX_PIN = 17;                 // to its DI
constexpr int DE_PIN = 16;                 // to its DE, with 10 kΩ to GND so the driver stays off while the S3 resets
constexpr int BAUD = 250000;               // measured on the bus; the community code's 256,000 is 2.4% off
constexpr int64_t BYTE_US = 40;            // start + 8 data + stop bits at 250,000 baud
constexpr int64_t QUIET_US = 200;          // before sending; the MK3 waits 67-90 µs, so it wins any tie
constexpr int RX_TIMEOUT_BYTES = 2;        // idle time, in byte times, that ends a received chunk
constexpr size_t MAX_FRAME = 300;          // longer than any frame seen; without an 0xFF, start over
constexpr int64_t STATS_US = 10000000;
constexpr unsigned long OUT_BAUD = 921600; // UART0 output; the bus's text needs about a tenth of it
constexpr int64_t POLICY_US = 500000;      // the ESS settings decide twice a second
constexpr int64_t SOC_FRESH_US = 60000000, POWER_FRESH_US = 5000000, SETPOINT_FRESH_US = 13000000;
constexpr time_t CLOCK_SET_AFTER = 1767225600;  // 2026-01-01: before this, NTP hasn't set the clock
constexpr uint32_t MEMORY_SAVE_MS = 60000;      // BatteryLife's memory goes to flash at most this often
constexpr uint32_t ESS_SAVE_MS = 60000;         // and so do settings changed by Home Assistant
constexpr size_t ESS_SETTINGS_BYTES = offsetof(ess::Settings, voltage_hold_cv);
static_assert(ESS_SETTINGS_BYTES == 164, "Preserve the v1 ESS settings blob for firmware rollback");
constexpr uint32_t VOLTAGE_HOLD_LATCHED = 1u << 16;

QueueHandle_t events;
vebus::Decoder decoder;
vebus::Requester requester;
acinput::Runtime ac_input;
pvdisable::Runtime pv_disable;
std::unique_ptr<upload::Uploader> uploader;  // allocated only while a VEConfigure file is loaded
SemaphoreHandle_t state_lock;  // decoder, requester, uploader and ESS policy: run by the bus task, read by the web server
constexpr int64_t READ_FRESH_US = 300000000;  // a file is shown against a read at most 5 minutes old

ess::Settings ess_settings;  // as saved in flash; changed by the page or UART0 (listener_ess_set)
ess_lease::Lease transient_charge;
external_battery::Feed battery_input;
constexpr char TIME_ZONE_KEY[] = "time-zone";
const controller_time::Zone *time_zone = nullptr;
bool manual_time_zone = false;
ess::Policy policy;
ess::Inputs ess_inputs;      // the policy's latest inputs and decision, for the page
ess::Decision decision;
int64_t policy_sample_at = -1;  // diagnostics: time of the cached policy decision
bool decide_now = false;     // settings changed: decide again at once
bool memory_dirty = false;   // BatteryLife's memory changed and waits to be saved
std::atomic<bool> voltage_hold_dirty{false};
Preferences prefs;           // flash; used from the Arduino loop task only
warnings::History warning_history; // sampled and served only by the Arduino loop task
warnings::Live warning_live;
warnings::InputWatch input_watch;
constexpr char WARNING_HISTORY_KEY[] = "warnhist1";
uint32_t system_boot_id = 0;
int64_t system_restart_at = -1;
bool system_restart_wifi = false;
bool system_restart_firmware = false;
bool system_restart_shutdown = false;
const char *system_restart_message = "Ready";
ha::Settings mqtt_settings;  // as saved in flash; changed on the MQTT page (listener_mqtt_set)
bool ess_dirty = false;      // ESS settings changed by Home Assistant and not yet saved
uint32_t ess_saved_ms = 0;

struct Command {
    enum Kind : uint8_t { TEST, CONTROL_ON, CONTROL_OFF, METER_PROBE } kind;
    vebus::Requester::Mode mode;  // TEST
    int32_t seconds;              // TEST; 0 = off
    int16_t watts;                // TEST in ESS mode
};
QueueHandle_t commands;  // from the Arduino loop, which reads UART0, to the bus task

struct {
    uint32_t frames, bytes, oversize, frame_err, parity_err, fifo_ovf, buf_full, breaks;
} stats;

uint8_t frame[MAX_FRAME];
size_t frame_len = 0;
int64_t frame_t0 = 0;
char line[24 + 3 * MAX_FRAME];
char report[1024];

// Status lines go to UART0 and are copied to the native USB port.
void write_both(const char *text, size_t n) {
    Serial.write(reinterpret_cast<const uint8_t *>(text), n);
    Serial0.write(reinterpret_cast<const uint8_t *>(text), n);
}

void say(const char *fmt, ...) {
    char text[400];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(text, sizeof text, fmt, args);
    va_end(args);
    if (n <= 0) return;
    if (n >= (int)sizeof text) n = sizeof text - 1;
    write_both(text, n);
}

// The requester's lines about its requests; call with state_lock held, print after releasing it.
bool take_report() { return requester.take_report(report, sizeof report); }

void emit() {
    uint8_t payload[3];  // inspect only the command prefix; never copy credentials into the log buffer
    size_t pn = vebus::payload_of(frame, frame_len, payload, sizeof payload);
    bool access = pn >= 3 && payload[0] == 0x00 && payload[2] == 0x3A;
    int n;
    if (access) {
        n = snprintf(line, sizeof line, "# bus %lld access credential [redacted]\n", (long long)frame_t0);
        vebus::erase_secret(payload, sizeof payload);
    } else {
        n = snprintf(line, sizeof line, "%lld", (long long)frame_t0);
        for (size_t i = 0; i < frame_len; i++) n += snprintf(line + n, sizeof line - n, " %02X", frame[i]);
        line[n++] = '\n';
    }
    Serial0.write(reinterpret_cast<const uint8_t *>(line), n);
    stats.frames++;
    xSemaphoreTake(state_lock, portMAX_DELAY);
    bool counted = false;
    bool own = requester.heard(frame, frame_len, frame_t0, &counted);
    if (decoder.set_ess_register(requester.ess_setpoint_ram())) decide_now = true;
    if (counted) {  // our last frame went by unheard, and this one shows it was taken: decode it too
        size_t n_sent = 0;
        int64_t t_sent = 0;
        const uint8_t *sent = requester.sent_frame(&n_sent, &t_sent);
        decoder.feed(sent, n_sent, t_sent, true);
    }
    decoder.feed(frame, frame_len, frame_t0, own);
    requester.observe(decoder.state(), frame_t0);
    bool any = take_report();
    xSemaphoreGive(state_lock);
    if (any) write_both(report, strlen(report));
}

// Add n received bytes, the last of which finished at t_end; earlier ones are taken as back to back.
void take(const uint8_t *buf, int n, int64_t t_end) {
    for (int i = 0; i < n; i++) {
        if (frame_len == MAX_FRAME) {
            stats.oversize++;
            frame_len = 0;
        }
        if (frame_len == 0) frame_t0 = t_end - (int64_t)(n - i) * BYTE_US;
        frame[frame_len++] = buf[i];
        if (buf[i] == 0xFF) {
            emit();
            frame_len = 0;
        }
    }
    stats.bytes += n;
}

void restart_rx() {
    uart_flush_input(BUS);
    xQueueReset(events);
    frame_len = 0;
}

// ---------------------------------------------------------------- sending

portMUX_TYPE tx_lock = portMUX_INITIALIZER_UNLOCKED;
enum Attempt { SENT, BUSY, LATE };

// Nothing on the line, and nothing half received.
IRAM_ATTR bool bus_quiet() {
    bool idle_level = !VEBUS_INVERT_RX;
    return (bool)((GPIO.in >> RX_PIN) & 1) == idle_level && UART1.fsm_status.st_urx_out == 0 &&
           UART1.status.rxfifo_cnt == 0;
}

// Put n bytes in UART1's FIFO and wait for the stop bit of the last one.
IRAM_ATTR void send_bytes(const uint8_t *b, size_t n) {
    uart_ll_write_txfifo(&UART1, b, n);
    int64_t busy_until = esp_timer_get_time() + (int64_t)n * BYTE_US - 2;  // it can't be done before this
    while (esp_timer_get_time() < busy_until) {
    }
    int64_t limit = busy_until + 100;
    while (!uart_ll_is_tx_idle(&UART1)) {
        if (esp_timer_get_time() > limit) {  // never seen; the driver goes off regardless
            uart_ll_txfifo_rst(&UART1);
            break;
        }
    }
}

// Wait until QUIET_US before the frame is due to start, watching the bus; then, once it has been quiet
// for QUIET_US, send the frame with the driver on for exactly its length. Gives up as soon as anyone
// else transmits; the caller reads their frame and decides again. Interrupts on this core stay off
// for the last look at the bus and the frame (at most about 0.75 ms), so the bus is watched without a
// break and no bytes reach the UART driver's buffer unread: the frame number stays the latest.
__attribute__((noinline)) IRAM_ATTR Attempt transmit(const uint8_t *f, size_t n, int64_t earliest_start,
                                                     int64_t latest_start, int64_t &t_start, int64_t &t_end) {
    while (esp_timer_get_time() < earliest_start - QUIET_US) {
        size_t waiting = 0;
        uart_get_buffered_data_len(BUS, &waiting);
        if (!bus_quiet() || waiting) return BUSY;
    }
    portENTER_CRITICAL(&tx_lock);
    int64_t quiet_from = esp_timer_get_time();
    size_t unread = 0;
    uart_get_buffered_data_len(BUS, &unread);  // handed over by the interrupt since the caller looked
    Attempt why = quiet_from + QUIET_US > latest_start ? LATE : unread ? BUSY : SENT;
    while (why == SENT && esp_timer_get_time() - quiet_from < QUIET_US) {
        if (!bus_quiet()) why = BUSY;
    }
    if (why != SENT) {
        portEXIT_CRITICAL(&tx_lock);
        return why;
    }
    GPIO.out_w1ts = 1u << DE_PIN;  // driver on, holding the idle 1
    esp_rom_delay_us(2);
    t_start = esp_timer_get_time();
    send_bytes(f, 2);  // the address,
    // the 2-bit pause Victron's senders leave (restarting the UART adds about 1 µs: 8 µs here measured
    // 2.24 bits on 2026-10-26),
    esp_rom_delay_us(vebus::Requester::ADDRESS_PAUSE_US - 1);
    send_bytes(f + 2, n - 2);  // and the rest
    GPIO.out_w1tc = 1u << DE_PIN;  // driver off straight after the stop bit of the 0xFF
    t_end = esp_timer_get_time();
    portEXIT_CRITICAL(&tx_lock);
    return SENT;
}

// Called whenever the bus task has caught up with everything received.
void try_request() {
    if (frame_len != 0) return;  // a frame is still coming in
    size_t buffered = 0;
    uart_get_buffered_data_len(BUS, &buffered);
    if (buffered) return;
    static uint8_t f[48];  // a 15-byte payload (a record chunk), escaped at worst
    size_t n = 0;
    int64_t earliest = 0, latest = 0;
    xSemaphoreTake(state_lock, portMAX_DELAY);
    bool due = requester.due(esp_timer_get_time(), f, sizeof f, &n, &earliest, &latest);
    xSemaphoreGive(state_lock);
    if (!due) return;
    int64_t t_start = 0, t_end = 0;
    Attempt result = transmit(f, n, earliest, latest, t_start, t_end);
    xSemaphoreTake(state_lock, portMAX_DELAY);
    if (result == SENT) {
        requester.sent(f, n, t_start, t_end);
    } else {
        requester.gave_up(result == LATE);
    }
    xSemaphoreGive(state_lock);
}

// ---------------------------------------------------------------- ESS from the settings

bool fresh(const vebus::Reading &r, int64_t now, int64_t max_age) { return r.t != vebus::NEVER && now - r.t < max_age; }

// Called with state_lock held. Only later inverter readback can verify a SOC
// broadcast; the outgoing broadcast itself is not measured inverter feedback.
external_battery::View battery_view(int64_t now) {
    auto out = battery_input.view(now);
    const auto &sync = requester.soc_sync(); const auto &soc = decoder.state().soc;
    out.sync_target = sync.target_soc; out.sync_sent = sync.soc_sent; out.sync_current_sent = sync.current_sent;
    out.sync_age_s = sync.last_soc<0 || now<sync.last_soc ? -1 : float(now-sync.last_soc)/1e6f;
    out.sync_verified = out.soc_valid && sync.last_soc>=0 && out.sync_age_s>=0 && out.sync_age_s<15 &&
        soc.t>sync.last_soc && now>=soc.t && fresh(soc,now,15000000) && fabsf(soc.v-sync.target_soc)<=0.5f;
    return out;
}

// With state_lock held: decide from the decoded state, hand the result to the requester, and put a
// line in `line` when what is decided changes in kind (running, state, schedule, flag word).
bool run_policy(int64_t now, char *line, size_t cap) {
    policy_sample_at = now;
    const vebus::State &st = decoder.state();
    const int64_t control_epoch = requester.controls_invalidated_at();
    const bool firmware_ready = !requester.firmware_gate(now);
    const bool ess_candidate = !requester.ess_firmware_gate(now);
    const auto *ess_model = ess_candidate ? multiplus_ii::find(requester.inverter_firmware()) : nullptr;
    const bool hold_voltage_ok = !ess_model || ess::hold_voltage_matches_model(ess_settings.voltage_hold_cv,
                                                                               ess_model->nominal_voltage);
    ess::Inputs in;
    in.now = now;
    time_t t = time(nullptr);
    in.clock_valid = t > CLOCK_SET_AFTER;
    in.utc = t;
    if (in.clock_valid && !(time_zone && controller_time::local(*time_zone, t, in.local))) {
        localtime_r(&t, &in.local);
    }
    in.soc_valid = st.soc.t >= control_epoch && fresh(st.soc, now, SOC_FRESH_US);
    in.soc = st.soc.v;
    const auto battery = battery_input.view(now);
    if (battery.soc_valid) { in.soc_valid = true; in.soc = battery.soc; }
    if (battery.enabled || battery_input.synchronization_started()) {
        requester.external_battery(battery.soc_valid, battery.soc,
                                   battery.current_valid ? battery.external_current_a : 0);
    }
    in.power_valid = st.acout_w.t >= control_epoch && fresh(st.acout_w, now, POWER_FRESH_US);
    in.acout_w = st.acout_w.v;
    if (st.bat_v.t >= control_epoch && fresh(st.bat_v, now, POWER_FRESH_US)) {
        in.bat_v = st.bat_v.v;
        in.voltage_at = st.bat_v.t;
    }
    // Prefer our last write, then the latest observed command or a bounded RAM readback.
    int last = requester.last_written();
    if (last == vebus::Requester::NOT_WRITTEN) {
        const auto &readback = st.setpoint_readback_w;
        const auto &setpoint = readback.t > st.setpoint_w.t && fabsf(readback.v) <= vebus::Requester::MAX_CTL_WATTS
            ? readback : st.setpoint_w;
        last = setpoint.t >= control_epoch && fresh(setpoint, now, SETPOINT_FRESH_US) ? (int)lroundf(setpoint.v) : 0;
    }
    const ess::Settings effective = transient_charge.effective(ess_settings, now);
    in.charge_window = transient_charge.charge_window(ess_settings, now);
    in.grid_hold = transient_charge.active(now) && effective.max_charge_w == 0;
    ess_inputs = in;
    const bool was_latched = policy.voltage_latched();
    if (!hold_voltage_ok) {
        decision = {};
        snprintf(decision.why, sizeof decision.why, "Battery voltage hold must be 19 to 33 V or off on a 24 V inverter");
    } else {
        decision = policy.decide(effective, in, last);
    }
    if (policy.voltage_latched() != was_latched) voltage_hold_dirty = true;
    acinput::Inputs ai;
    ai.now=now;
    auto cs=requester.control_state(now);
    const bool transfer_pending = requester.ignore_ac_transfer_pending(now);
    static int64_t last_control_epoch = -1;
    if (last_control_epoch != control_epoch) {
        last_control_epoch = control_epoch;
        if (ac_input.requested && !transfer_pending) ac_input.cancel("VE.Bus changed: accepting grid");
        if (pv_disable.requested) pv_disable.cancel("VE.Bus changed: restoring PV operation");
    }
    ai.owner = (firmware_ready || transfer_pending) &&
        (cs == vebus::Requester::CtlState::POLLING || cs == vebus::Requester::CtlState::CHECKING ||
         cs == vebus::Requester::CtlState::WRITING);
    ai.busy=requester.settings_busy() || requester.on();
    ai.ess_running=ess_settings.enabled;
    ai.mode_on=ac_input.mode_on(st.actual_mode,st.actual_mode_t,now);
    ai.telemetry=st.bus_fresh(now) && fresh(st.acout_w,now,POWER_FRESH_US);
    ai.soc_valid=battery.soc_valid || fresh(st.soc,now,15000000);
    ai.soc=battery.soc_valid ? battery.soc : st.soc.v;
    ai.soc_source=battery.soc_valid ? "External battery" : "Inverter";
    ai.voltage_valid=fresh(st.bat_v,now,POWER_FRESH_US);ai.voltage=st.bat_v.v;
    ai.soc_floor=fmaxf(ess_settings.min_soc,decision.active_soc_limit);
    ai.main_state=st.device_state;ai.main_at=st.device_state_t;
    ai.overload=(in.power_valid && fabsf(in.acout_w)>4000) ||
        (st.device_state_t>=0 && now-st.device_state_t<3000000 && (st.device_state<4 || st.device_state>9));
    ai.voltage_hold = decision.voltage_hold;
    ai.charging=decision.schedule>=0 || decision.charging || transient_charge.active(now) ||
        ess_settings.mode==ess::Mode::KEEP_CHARGED || system_restart_at>=0;
    bool timed = transient_charge.active(now);
    for(const auto &slot:ess_settings.schedule)timed=timed||slot.enabled;
    ai.clock_ready=!timed || in.clock_valid;
    ac_input.update(ai,requester.ac_input_wire());
    pv_disable.update(ac_input,requester.pv_wire(),now);
    requester.pv_target(pv_disable.requested,now);
    requester.ac_input_target(ac_input.requested);
    // Stopped, the flag word goes back to normal; the requester writes it only once the check read
    // has found it, which needs ESS to have run from here since the last restart.
    int normal = ess_settings.dc_pv_feed_in ? 0 : ess::FLAG_NO_DC_PV_FEED_IN;
    const bool ess_allowed = ess_candidate && hold_voltage_ok;
    const bool write = ess_allowed && (control_epoch < 0 || in.power_valid) && decision.write;
    requester.target(write, decision.setpoint_w, ess_allowed ? (write ? decision.flags : normal) : -1);
    if (policy.changed()) memory_dirty = true;
    static uint32_t last_kind = 0xFFFFFFFF;
    const ess::Decision &d = decision;
    uint32_t kind = (uint32_t)d.write | (uint32_t)d.state << 1 | (uint32_t)(d.schedule + 1) << 5 |
                    (uint32_t)d.charging << 8 | (uint32_t)d.flags << 9 | (uint32_t)ess_settings.mode << 13 |
                    (uint32_t)d.voltage_hold << 15;
    if (kind == last_kind) return false;
    last_kind = kind;
    snprintf(line, cap, "# ess: %s, %s, state %u (%s): setpoint %+d W (target %+d W), flag word 0x%02X: %s\n",
             d.write ? "running" : "stopped", ess::mode_name(ess_settings.mode), d.state, ess::state_name(d.state),
             d.setpoint_w, d.target_w, d.flags, d.why);
    return true;
}

void save_settings(const ess::Settings &s) { prefs.putBytes("settings", &s, ESS_SETTINGS_BYTES); }

bool save_time_zone(const controller_time::Zone *zone, bool manual) {
    char record[64];
    const int length = snprintf(record, sizeof record, "%c:%s", manual ? 'm' : 'a',
                                zone ? controller_time::name(*zone) : "");
    return length > 0 && size_t(length) < sizeof record &&
           prefs.putString(TIME_ZONE_KEY, record) == size_t(length);
}

void save_mqtt(const ha::Settings &s) { prefs.putBytes("mqtt", &s, sizeof s); }

void load_settings() {
    battery_input.enabled(prefs.getBool("extbat", false));
    acinput::Config ac;
    if(prefs.getBytesLength("acinput")==sizeof ac && prefs.getBytes("acinput",&ac,sizeof ac)==sizeof ac && acinput::valid(ac))ac_input.config=ac;
    pvdisable::Config pv;
    if(prefs.getBytesLength("pvdisable")==sizeof pv && prefs.getBytes("pvdisable",&pv,sizeof pv)==sizeof pv && pvdisable::valid(pv))pv_disable.config=pv;
    ess::Settings s;
    if (prefs.getBytesLength("settings") == ESS_SETTINGS_BYTES &&
        prefs.getBytes("settings", &s, ESS_SETTINGS_BYTES) == ESS_SETTINGS_BYTES &&
        s.version == ess::Settings().version && ess::validate(s, nullptr, 0)) {
        ess_settings = s;
    }
    if (prefs.isKey(TIME_ZONE_KEY)) {
        const String record = prefs.getString(TIME_ZONE_KEY);
        if (record.length() >= 2 && record[1] == ':' && (record[0] == 'a' || record[0] == 'm')) {
            time_zone = controller_time::find(record.c_str() + 2);
            manual_time_zone = record[0] == 'm' && (time_zone || record.length() == 2);
        }
    }
    const uint32_t hold = prefs.getUInt("volthold", 0);
    const uint16_t cv = uint16_t(hold);
    if (!(hold & 0xfffe0000u) && ess::valid_hold_voltage(cv)) {
        ess_settings.voltage_hold_cv = cv;
        policy.restore_voltage_hold(cv, (hold & VOLTAGE_HOLD_LATCHED) != 0);
    }
    ess_settings.schedule_grid_mask = prefs.getUChar("schedgrid", 0) & ((1u << ess::SCHEDULES) - 1);
    ess::Memory m;
    if (prefs.getBytesLength("memory") == sizeof m && prefs.getBytes("memory", &m, sizeof m) == sizeof m &&
        m.version == ess::Memory().version) {
        policy.memory = m;
    }
    ha::Settings q;
    if (prefs.getBytesLength("mqtt") == sizeof q && prefs.getBytes("mqtt", &q, sizeof q) == sizeof q &&
        q.version == ha::Settings().version && ha::validate(q, nullptr, 0)) {
        mqtt_settings = q;
    }
}

// ---------------------------------------------------------------- the bus task

void print_stats(int64_t now) {
    char wifi[96], tx[200], ess_text[64];
    web::describe(wifi, sizeof wifi);
    xSemaphoreTake(state_lock, portMAX_DELAY);
    requester.describe(now, tx, sizeof tx);
    snprintf(ess_text, sizeof ess_text, "ess=%s state=%u sp=%+d target=%+d flags=%u", ess_settings.enabled ? "run" : "stop",
             decision.state, decision.setpoint_w, decision.target_w, decision.flags);
    xSemaphoreGive(state_lock);
    say("# %lld frames=%lu bytes=%lu frame_err=%lu parity_err=%lu fifo_ovf=%lu buf_full=%lu breaks=%lu oversize=%lu "
        "%s %s %s\n",
        (long long)now, (unsigned long)stats.frames, (unsigned long)stats.bytes, (unsigned long)stats.frame_err,
        (unsigned long)stats.parity_err, (unsigned long)stats.fifo_ovf, (unsigned long)stats.buf_full,
        (unsigned long)stats.breaks, (unsigned long)stats.oversize, tx, ess_text, wifi);
}

void bus_task(void *) {
    static uint8_t buf[256];
    uart_event_t ev;
    int64_t next_stats = esp_timer_get_time() + STATS_US, next_policy = 0;
    static char ess_line[320];
    for (;;) {
        if (xQueueReceive(events, &ev, pdMS_TO_TICKS(50))) {
            int64_t now = esp_timer_get_time();
            switch (ev.type) {
            case UART_DATA: {
                // A chunk that ended on the idle timeout finished RX_TIMEOUT_BYTES byte times ago;
                // one handed over because the FIFO filled finished just now.
                int64_t chunk_end = ev.timeout_flag ? now - RX_TIMEOUT_BYTES * BYTE_US : now;
                size_t left = ev.size;
                while (left > 0) {
                    int n = uart_read_bytes(BUS, buf, left < sizeof buf ? left : sizeof buf, 0);
                    if (n <= 0) break;
                    left -= n;
                    take(buf, n, chunk_end - (int64_t)left * BYTE_US);
                }
                break;
            }
            case UART_FIFO_OVF:
                stats.fifo_ovf++;
                restart_rx();
                break;
            case UART_BUFFER_FULL:
                stats.buf_full++;
                restart_rx();
                break;
            case UART_FRAME_ERR:
                stats.frame_err++;
                break;
            case UART_PARITY_ERR:
                stats.parity_err++;
                break;
            case UART_BREAK:
                stats.breaks++;
                break;
            default:
                break;
            }
        }
        int64_t now = esp_timer_get_time();
        Command c;
        xSemaphoreTake(state_lock, portMAX_DELAY);
        while (xQueueReceive(commands, &c, 0)) {
            switch (c.kind) {
            case Command::TEST: requester.start(now, c.seconds, c.mode, c.watts); break;
            case Command::CONTROL_ON: requester.control(true); break;
            case Command::CONTROL_OFF: requester.control(false); break;
            case Command::METER_PROBE:
                requester.probe_meters(now);
                break;
            }
        }
        bool ess_changed = false;
        if (now >= next_policy || decide_now) {
            decide_now = false;
            next_policy = now + POLICY_US;
            ess_changed = run_policy(now, ess_line, sizeof ess_line);
        }
        requester.tick(now);
        if (uploader) uploader->step(requester, now, (int64_t)time(nullptr));
        requester.watch_charger(now);
        if (decoder.set_ess_register(requester.ess_setpoint_ram())) decide_now = true;
        bool any = take_report();
        xSemaphoreGive(state_lock);
        if (ess_changed) write_both(ess_line, strlen(ess_line));
        if (any) write_both(report, strlen(report));
        try_request();
        if (now >= next_stats) {
            print_stats(now);
            next_stats += STATS_US;
        }
    }
}

// Lines from the Pi on UART0. Unprintable bytes are dropped, and a line left unfinished for 200 ms is
// forgotten: junk from a glitch on the port once filled the buffer and swallowed the next command.
void read_commands() {
    static char cmd[40];
    static size_t len = 0;
    static uint32_t last_ms = 0;
    while (Serial0.available()) {
        int ch = Serial0.read();
        uint32_t now_ms = millis();
        if (now_ms - last_ms > 200) len = 0;
        last_ms = now_ms;
        if (ch != '\n' && ch != '\r') {
            if (ch >= 0x20 && ch < 0x7F && len < sizeof cmd - 1) cmd[len++] = (char)ch;
            continue;
        }
        cmd[len] = 0;
        if (len == 0) continue;
        len = 0;
        if (strcmp(cmd, "wifi reconnect") == 0) {
            web::reconnect();
            continue;
        }
        if (strcmp(cmd, "wifi reset") == 0) {
            say("# wifi: %s\n", web::reset_wifi() ? "restoring provisioned network" : "could not restore provisioned network");
            continue;
        }
        if (strcmp(cmd, "web password reset") == 0) {
            say("# web: %s\n", web::reset_password() ? "password and API token cleared" : "could not clear password");
            continue;
        }
        Command c = {Command::TEST, vebus::Requester::Mode::OFF, 0, 0};
        long s = 0, w = 0;
        bool arm = sscanf(cmd, "ctl arm %ld", &w) == 1;
        if (arm || strcmp(cmd, "ctl disarm") == 0) {  // the same as on the ESS settings page
            web::EssView v;
            listener_ess(v);
            v.settings.enabled = arm;
            if (arm) v.settings.grid_setpoint_w = (int)-w;
            char why[120];
            if (!listener_ess_set(v.settings, why, sizeof why, true)) {
                say("# control: refused: %s\n", why);
            } else if (arm) {
                say("# control: ESS from here, grid setpoint %+ld W (+ = export)\n", w);
            } else {
                say("# control: ESS from here off\n");
            }
            continue;
        }
        if (strcmp(cmd, "tx off") == 0 || strcmp(cmd, "bench off") == 0 || strcmp(cmd, "ess off") == 0) {
            c.seconds = 0;
        } else if (strcmp(cmd, "meters") == 0) {
            c.kind=Command::METER_PROBE;
        } else if (strcmp(cmd, "ctl on") == 0) {
            c.kind = Command::CONTROL_ON;
        } else if (strcmp(cmd, "ctl off") == 0) {
            c.kind = Command::CONTROL_OFF;
        } else if (sscanf(cmd, "tx %ld", &s) == 1) {
            c.seconds = s;
            c.mode = vebus::Requester::Mode::READ;
        } else if (sscanf(cmd, "bench %ld", &s) == 1) {
            c.seconds = s;
            c.mode = vebus::Requester::Mode::BENCH;
        } else if (sscanf(cmd, "ess %ld %ld", &w, &s) == 2 && w >= -32768 && w <= 32767) {
            c.seconds = s;
            c.mode = vebus::Requester::Mode::ESS;
            c.watts = (int16_t)w;  // the requester refuses anything beyond +-100 W
        } else {
            say("# unknown command \"%s\": use meters, ctl arm <watts>, ctl disarm, ctl on, ctl off, tx <seconds>, "
                "bench <seconds>, ess <watts> <seconds>, tx off, wifi reconnect, wifi reset, or web password reset\n",
                cmd);
            continue;
        }
        xQueueSend(commands, &c, 0);
    }
}

}  // namespace

// The ESS settings and what they make the controller do, for the pages and UART0.
void listener_ess(web::EssView &v) {
    xSemaphoreTake(state_lock, portMAX_DELAY);
    v.settings = ess_settings;
    v.decision = decision;
    v.memory = policy.memory;
    const int64_t now = esp_timer_get_time();
    const auto &st = decoder.state();
    v.ctl = requester.control_state_name(now);
    if (v.settings.enabled) {
        if (const char *reason = requester.ess_firmware_gate(now))
            snprintf(v.decision.why, sizeof v.decision.why, "%s", reason);
    }
    v.multi_flags = requester.multi_flags();
    v.inverter = requester.inverter_view(now, st);
    v.voltage_valid = fresh(st.bat_v, now, ess::HOLD_SAMPLE_MAX_US) && isfinite(st.bat_v.v) && st.bat_v.v > 0;
    v.voltage = st.bat_v.v;
    v.voltage_hold_confirmed = v.decision.voltage_hold && ac_input.grid == 1 &&
        requester.control_state(now) == vebus::Requester::CtlState::WRITING && st.discharge_disabled(now) == 1 &&
        (int(st.flags.v) & ess::FLAG_NO_CHARGE);
    v.soc_valid = ess_inputs.soc_valid;
    v.soc = ess_inputs.soc;
    v.clock_valid = ess_inputs.clock_valid;
    v.detect_time_zone = !time_zone && !manual_time_zone;
    v.time_zone = time_zone ? controller_time::name(*time_zone) : "";
    if (v.clock_valid) {
        strftime(v.clock, sizeof v.clock, time_zone ? "%Y-%m-%d %H:%M:%S" : "%Y-%m-%d %H:%M:%S %Z",
                 &ess_inputs.local);
    } else {
        snprintf(v.clock, sizeof v.clock, "not set yet (NTP)");
    }
    xSemaphoreGive(state_lock);
}

// Only the Arduino loop reads or changes the stored choice. The bus task uses
// the zone under state_lock when calculating the next ESS setpoint.
const char *listener_time_zone(bool &manual) {
    manual = manual_time_zone;
    return time_zone ? controller_time::name(*time_zone) : "";
}

bool listener_time_zone_set(const controller_time::Zone *zone, bool manual, bool first_detection) {
    if (first_detection && (time_zone || manual_time_zone)) return true;
    if (!zone) zone = time_zone;  // Keep the current rule if the browser could not supply a zone.
    if (zone == time_zone && manual == manual_time_zone) return true;
    if (!save_time_zone(zone, manual)) return false;
    xSemaphoreTake(state_lock, portMAX_DELAY);
    if (zone != time_zone) {
        time_zone = zone;
        policy.reset_schedule_hold();
        decide_now = true;
    }
    manual_time_zone = manual;
    xSemaphoreGive(state_lock);
    return true;
}

// One-shot inverter commands from the web page or MQTT; the bus task sends and verifies them.
bool listener_inverter_command(const char *key, const char *value, char *why, size_t cap) {
    inverter::Command c;
    if (!inverter::parse(key, value, c, why, cap)) return false;
    xSemaphoreTake(state_lock, portMAX_DELAY);
    bool ok = requester.operate(esp_timer_get_time(), c, decoder.state(), why, cap);
    xSemaphoreGive(state_lock);
    return ok;
}

void listener_charger(charger::View &v) {
    xSemaphoreTake(state_lock, portMAX_DELAY);
    v = requester.charger_view(esp_timer_get_time());
    xSemaphoreGive(state_lock);
}

bool listener_charger_read(char *why, size_t cap) {
    xSemaphoreTake(state_lock, portMAX_DELAY);
    bool ok = requester.read_charger(esp_timer_get_time(), why, cap);
    xSemaphoreGive(state_lock);
    return ok;
}

bool listener_charger_set(const charger::Change &change, char *why, size_t cap) {
    xSemaphoreTake(state_lock, portMAX_DELAY);
    bool ok = requester.set_charger(esp_timer_get_time(), change, why, cap);
    xSemaphoreGive(state_lock);
    return ok;
}

// Explicit PowerAssist reads share the existing serialized settings engine.
// Cached GETs do not schedule any bus traffic.
void listener_assist(charger::View &v) {
    xSemaphoreTake(state_lock, portMAX_DELAY);
    v = requester.charger_view(esp_timer_get_time(), charger::Profile::ASSIST);
    xSemaphoreGive(state_lock);
}
bool listener_assist_read(char *why, size_t cap) {
    xSemaphoreTake(state_lock, portMAX_DELAY);
    bool ok = requester.read_charger(esp_timer_get_time(), why, cap, charger::Profile::ASSIST);
    xSemaphoreGive(state_lock);
    return ok;
}
bool listener_assist_set(const charger::Change &change, char *why, size_t cap) {
    xSemaphoreTake(state_lock, portMAX_DELAY);
    bool ok = requester.set_charger(esp_timer_get_time(), change, why, cap, charger::Profile::ASSIST);
    xSemaphoreGive(state_lock);
    return ok;
}

void listener_system(uint32_t &boot_id, bool &pending, const char *&message) {
    boot_id = system_boot_id; pending = system_restart_at >= 0; message = system_restart_message;
}
size_t listener_ac_input_json(char *out,size_t cap) {
    acinput::Runtime r;acinput::Wire w;
    xSemaphoreTake(state_lock,portMAX_DELAY);r=ac_input;w=requester.ac_input_wire();r.latest.now=esp_timer_get_time();xSemaphoreGive(state_lock);
    return acinput::to_json(r,w,out,cap);
}
size_t listener_pv_json(char *out,size_t cap){
    pvdisable::Runtime r;pvdisable::Wire w;
    xSemaphoreTake(state_lock,portMAX_DELAY);r=pv_disable;w=requester.pv_wire();r.now=esp_timer_get_time();xSemaphoreGive(state_lock);
    return pvdisable::to_json(r,w,out,cap);
}
bool listener_pv_request(bool disable,uint32_t base,char *why,size_t cap){
    xSemaphoreTake(state_lock,portMAX_DELAY);
    const int64_t now = esp_timer_get_time();
    const char *blocked = disable ? requester.firmware_gate(now) : nullptr;
    const auto &wire = requester.pv_wire();
    if (!blocked && disable && !wire.wanted && !wire.ready(now))
        blocked = "Wait for fresh PV flag readback and idle controls";
    const bool ok = !blocked && pv_disable.request(disable,base,why,cap);
    if (blocked) snprintf(why, cap, "%s", blocked);
    if(ok){requester.pv_target(pv_disable.requested,now,!disable);decide_now=true;}
    xSemaphoreGive(state_lock);return ok;
}
bool listener_pv_config(const pvdisable::Config &c,uint32_t base,char *why,size_t cap){
    if(!pvdisable::valid(c)){snprintf(why,cap,"Use a restoration duration of 10 to 300 seconds");return false;}
    xSemaphoreTake(state_lock,portMAX_DELAY);bool same=base==pv_disable.revision;xSemaphoreGive(state_lock);
    if(!same){snprintf(why,cap,"PV control changed; refresh before saving");return false;}
    if(prefs.putBytes("pvdisable",&c,sizeof c)!=sizeof c){snprintf(why,cap,"PV duration storage failed");return false;}
    xSemaphoreTake(state_lock,portMAX_DELAY);pv_disable.config=c;pv_disable.revision++;decide_now=true;xSemaphoreGive(state_lock);return true;
}
bool listener_ac_input_request(bool ignore,uint32_t base,char *why,size_t cap) {
    xSemaphoreTake(state_lock,portMAX_DELAY);
    const int64_t now = esp_timer_get_time();
    const char *blocked = ignore ? requester.firmware_gate(now) : nullptr;
    const auto &wire = requester.ac_input_wire();
    if (!blocked && ignore && !wire.wanted && !wire.ready(now))
        blocked = "Wait for confirmed grid connection, fresh SOC above floor + 2%, and idle controls";
    const bool ok = !blocked && ac_input.request(ignore,base,wire,why,cap);
    if (blocked) snprintf(why, cap, "%s", blocked);
    if(ok){requester.ac_input_target(ac_input.requested);decide_now=true;}
    xSemaphoreGive(state_lock);return ok;
}
bool listener_ac_input_config(const acinput::Config &c,uint32_t base,char *why,size_t cap) {
    if(!acinput::valid(c)){snprintf(why,cap,"Invalid reconnect protection settings");return false;}
    xSemaphoreTake(state_lock,portMAX_DELAY);bool same=base==ac_input.revision;xSemaphoreGive(state_lock);
    if(!same){snprintf(why,cap,"AC-input settings changed; reload before saving");return false;}
    if(prefs.putBytes("acinput",&c,sizeof c)!=sizeof c){snprintf(why,cap,"Protection settings storage failed");return false;}
    xSemaphoreTake(state_lock,portMAX_DELAY);ac_input.config=c;ac_input.revision++;decide_now=true;xSemaphoreGive(state_lock);
    return true;
}
static bool manual_restart_busy_locked(int64_t now) {
    return requester.settings_busy() || ac_input.requested || requester.ac_input_wire().actual(now) == 1 ||
           pv_disable.requested || (requester.pv_wire().supported && requester.pv_wire().actual(now) != 0);
}

bool listener_restart(char *why, size_t cap) {
    xSemaphoreTake(state_lock, portMAX_DELAY);
    const bool busy = manual_restart_busy_locked(esp_timer_get_time());
    xSemaphoreGive(state_lock);
    if (busy || system_restart_at >= 0) {
        snprintf(why, cap, "%s", busy ? "Reconnect AC input and wait for inverter operations to finish" : "Controller restart already pending");
        return false;
    }
    system_restart_at = esp_timer_get_time() + 1000000; // send the HTTP response first
    system_restart_message = "Controller restart queued";
    return true;
}

static bool wifi_restart_safe_locked(int64_t now) {
    const auto &s = decoder.state();
    const auto &ac_wire = requester.ac_input_wire();
    const auto &pv_wire = requester.pv_wire();
    return !requester.settings_busy() && !requester.on() &&
           !ac_input.requested && ac_wire.actual(now) == 0 &&
           !pv_disable.requested && (!pv_wire.supported || pv_wire.actual(now) == 0) &&
           ac_input.grid == 1 && warnings::fresh(ac_input.latest.now, now, 2000000) &&
           s.bus_fresh(now) && fresh(s.mains_v, now, POWER_FRESH_US) && s.mains_v.v > 100 &&
           s.actual_mode == 7 && warnings::fresh(s.actual_mode_t, now);
}

bool listener_wifi_restart() {
    if (system_restart_at >= 0) return system_restart_wifi;
    const int64_t now = esp_timer_get_time();
    xSemaphoreTake(state_lock, portMAX_DELAY);
    const bool safe = wifi_restart_safe_locked(now);
    xSemaphoreGive(state_lock);
    if (!safe) return false;
    system_restart_wifi = true;
    system_restart_shutdown = true; // The Wi-Fi loop starts the radio shutdown immediately.
    system_restart_at = now + int64_t(wifi_recovery::RESTART_WAIT_MS) * 1000;
    system_restart_message = "Restart queued after repeated Wi-Fi failures";
    say("# wifi: queued controller restart after repeated network recovery attempts\n");
    return true;
}

static bool firmware_safe_locked(int64_t now) {
    // A bench S3 may be updated before it has ever seen an inverter.
    const auto &s = decoder.state();
    const bool bench = s.last_multi < 0 && s.last_master < 0 && !requester.settings_busy() && !requester.on();
    return bench || wifi_restart_safe_locked(now);
}

bool listener_firmware_allowed() {
    xSemaphoreTake(state_lock, portMAX_DELAY);
    const bool safe = firmware_safe_locked(esp_timer_get_time());
    xSemaphoreGive(state_lock);
    return system_restart_at < 0 && safe;
}

const char *listener_firmware_restart(firmware_ota::Action action) {
    if (!listener_firmware_allowed()) return "Connect mains and wait for inverter operations to finish.";
    if (const char *why = firmware_ota::queue(action)) return why;
    system_restart_firmware = true;
    system_restart_at = esp_timer_get_time() + 1500000;
    system_restart_message = "Firmware restart queued";
    return nullptr;
}

// New ESS settings: saved at once, or (Home Assistant's commands) within ESS_SAVE_MS, to spare flash.
// From the Arduino loop task
// (page, UART0 or MQTT) only. False, with the reason, if any value is out of range.
bool listener_ess_set(const ess::Settings &s, char *why, size_t cap, bool save_now, bool legacy_zone) {
    if (!ess::validate(s, why, cap)) return false;
    const bool tz_changed = strcmp(s.tz, ess_settings.tz) != 0;
    const bool use_legacy_zone = legacy_zone && (tz_changed || time_zone);
    const bool voltage_changed = s.voltage_hold_cv != ess_settings.voltage_hold_cv;
    bool latched = false;
    if (voltage_changed) {
        xSemaphoreTake(state_lock, portMAX_DELAY);
        latched = s.voltage_hold_cv && policy.voltage_latched();
        xSemaphoreGive(state_lock);
        if (prefs.putUInt("volthold", s.voltage_hold_cv | (latched ? VOLTAGE_HOLD_LATCHED : 0)) != sizeof(uint32_t)) {
            snprintf(why, cap, "Battery voltage hold storage failed");
            return false;
        }
    }
    const bool schedule_grid_changed = s.schedule_grid_mask != ess_settings.schedule_grid_mask;
    if (schedule_grid_changed && prefs.putUChar("schedgrid", s.schedule_grid_mask) != sizeof(uint8_t)) {
        snprintf(why, cap, "Schedule grid setting storage failed");
        return false;
    }
    if (use_legacy_zone && (!manual_time_zone || time_zone) && !save_time_zone(nullptr, true)) {
        snprintf(why, cap, "Time zone storage failed");
        return false;
    }
    xSemaphoreTake(state_lock, portMAX_DELAY);
    if (voltage_changed) {
        policy.restore_voltage_hold(s.voltage_hold_cv, latched);
        voltage_hold_dirty = false;
    }
    bool schedule_changed = schedule_grid_changed || tz_changed || use_legacy_zone;
    for (int i = 0; i < ess::SCHEDULES; i++) {
        const auto &a = s.schedule[i];
        const auto &b = ess_settings.schedule[i];
        if (a.soc != b.soc) policy.schedule_changed();
        schedule_changed = schedule_changed || a.enabled != b.enabled || a.day != b.day ||
            a.start_s != b.start_s || a.duration_s != b.duration_s || a.soc != b.soc;
    }
    if (schedule_changed) policy.reset_schedule_hold();
    ess_settings = s;
    if (use_legacy_zone) {
        time_zone = nullptr;
        manual_time_zone = true;
    }
    if (tz_changed) {
        setenv("TZ", s.tz, 1);
        tzset();
    }
    decide_now = true;
    xSemaphoreGive(state_lock);
    if (save_now) {
        save_settings(s);
        ess_dirty = false;
        ess_saved_ms = millis();
    } else {
        ess_dirty = true;
    }
    return true;
}

// The MQTT settings, for the MQTT page; and new ones from it: checked, saved and put in force.
void listener_mqtt(ha::Settings &out) { out = mqtt_settings; }

bool listener_mqtt_set(const ha::Settings &s, char *why, size_t cap) {
    if (!ha::validate(s, why, cap)) return false;
    mqtt_settings = s;
    save_mqtt(s);
    mqtt_link::apply(s);
    return true;
}

void listener_battery_input(external_battery::View &out) {
    xSemaphoreTake(state_lock, portMAX_DELAY);
    out = battery_view(esp_timer_get_time());
    xSemaphoreGive(state_lock);
}

bool listener_battery_input_enabled(bool value, char *why, size_t cap) {
    external_battery::View current; listener_battery_input(current);
    if (current.enabled == value) return true;
    if (prefs.putBool("extbat", value) != 1) {
        snprintf(why, cap, "Could not save settings."); return false;
    }
    xSemaphoreTake(state_lock, portMAX_DELAY);
    battery_input.enabled(value); decide_now = true;
    xSemaphoreGive(state_lock);
    return true;
}

void listener_battery_input_connection(bool connected) {
    xSemaphoreTake(state_lock, portMAX_DELAY);
    battery_input.connected(connected);
    xSemaphoreGive(state_lock);
}

bool listener_battery_input_message(const char *payload, size_t size, bool retained,
                                    int64_t queued_at, const char **why) {
    xSemaphoreTake(state_lock, portMAX_DELAY);
    const bool ok = battery_input.accept(payload, size, retained, queued_at, esp_timer_get_time(),
                                          double(time(nullptr)), why);
    if (ok) decide_now = true;
    xSemaphoreGive(state_lock);
    return ok;
}

void listener_ess_lease(ess_lease::View &out) {
    xSemaphoreTake(state_lock, portMAX_DELAY);
    out = transient_charge.view(esp_timer_get_time());
    xSemaphoreGive(state_lock);
}
const char *listener_ess_lease_set(const char *id, uint32_t ttl_s, int charge_w) {
    xSemaphoreTake(state_lock, portMAX_DELAY);
    const char *error = transient_charge.set(id, ttl_s, charge_w, esp_timer_get_time());
    if (!error) decide_now = true;
    xSemaphoreGive(state_lock);
    return error;
}
const char *listener_ess_lease_clear(const char *id) {
    xSemaphoreTake(state_lock, portMAX_DELAY);
    const char *error = transient_charge.clear(id, esp_timer_get_time());
    if (!error) decide_now = true;
    xSemaphoreGive(state_lock);
    return error;
}

// Start a backup or read the connected MultiPlus-II's setup settings.
bool listener_settings_read(bool inspection) {
    xSemaphoreTake(state_lock, portMAX_DELAY);
    const bool ok = (!uploader || !uploader->busy()) && requester.read_settings(esp_timer_get_time(), inspection);
    xSemaphoreGive(state_lock);
    return ok;
}

void listener_settings(vebus::Requester::SettingsRead &out, web::InverterSetupView *setup) {
    xSemaphoreTake(state_lock, portMAX_DELAY);
    out = requester.settings();
    if (setup) {
        const int64_t now = esp_timer_get_time();
        const int64_t epoch = requester.controls_invalidated_at();
        const auto &state = decoder.state();
        const bool ready = requester.inspection_ready(now);
        const bool identity_live = state.bus_fresh(now) && state.inverter_firmware_at != vebus::NEVER &&
                                   state.inverter_firmware_at >= epoch && state.inverter_firmware_at <= now;
        const uint32_t firmware = ready ? out.firmware : identity_live ? state.inverter_firmware : 0;
        const auto *model = multiplus_ii::find(firmware);
        if (model) {
            setup->model = model->name;
            setup->nominal_voltage = model->nominal_voltage;
            setup->ess_setpoint_ram = requester.ess_discovered_ram(now);
        }
        setup->can_upload = !requester.settings_write_firmware_gate(now);
        if (out.inspection && out.active) setup->status = "reading";
        else if (ready) setup->status = "ready";
        else if (firmware && !model) setup->status = "unsupported";
        else if (out.inspection && out.done) {
            setup->status = out.started < epoch || !out.problem ? "stale" : "incomplete";
        }
    }
    xSemaphoreGive(state_lock);
}

// Writing a settings file (upload.h), from the Multi settings page. A file loaded is parsed; unless the
// Multi was read in the last 5 minutes a read starts, so the page can show what the file would change.
bool listener_upload_load(const uint8_t *data, size_t n, const char *name, char *why, size_t cap) {
    xSemaphoreTake(state_lock, portMAX_DELAY);
    if (!uploader) uploader.reset(new (std::nothrow) upload::Uploader);
    if (!uploader) {
        xSemaphoreGive(state_lock);
        snprintf(why, cap, "Not enough memory to load a VEConfigure file");
        return false;
    }
    bool ok = uploader->load(data, n, name, why, cap);
    if (!ok && uploader->state() == upload::EMPTY) {
        uploader->clear();
        uploader.reset();
    }
    int64_t now = esp_timer_get_time();
    const vebus::Requester::SettingsRead &r = requester.settings();
    if (ok && !r.active && (r.inspection || !r.done || now - r.finished > READ_FRESH_US)) requester.read_settings(now);
    xSemaphoreGive(state_lock);
    return ok;
}

// Starts the job: refused at once unless this ESP32 is the bus master; the job reads the Multi again,
// checks the file against that read, writes, and reads back.
bool listener_upload_start(char *why, size_t cap, bool access_only) {
    using CtlState = vebus::Requester::CtlState;
    xSemaphoreTake(state_lock, portMAX_DELAY);
    int64_t now = esp_timer_get_time();
    CtlState s = requester.control_state(now);
    const char *w = "no file loaded";
    bool ok = false;
    if (s == CtlState::PAUSED) {
        snprintf(why, cap, "another master (the MK3) is on the bus: stop Venus's VE.Bus service or unplug the MK3 first");
    } else if (s != CtlState::POLLING && s != CtlState::CHECKING && s != CtlState::WRITING) {
        snprintf(why, cap, "the controller is %s", requester.control_state_name(now));
    } else if (requester.settings_write_firmware_gate(now)) {
        snprintf(why, cap, "Use VEConfigure to change settings on this inverter firmware");
    } else if (!uploader || !(ok = uploader->start(requester, now, &w, access_only))) {
        snprintf(why, cap, "%s", w);
    }
    xSemaphoreGive(state_lock);
    return ok;
}

bool listener_upload_cancel(char *why, size_t cap) {
    const char *w = "nothing to call off";
    xSemaphoreTake(state_lock, portMAX_DELAY);
    bool ok = uploader && uploader->cancel(&w);
    xSemaphoreGive(state_lock);
    if (!ok) snprintf(why, cap, "%s", w);
    return ok;
}

void listener_upload_clear() {
    xSemaphoreTake(state_lock, portMAX_DELAY);
    if (uploader && !uploader->busy()) {
        uploader->clear();
        uploader.reset();
    }
    xSemaphoreGive(state_lock);
}

// Where it stands, with the file checked against the Multi's latest read (while writing: the check the
// job writes by).
void listener_upload(web::UploadView &v) {
    xSemaphoreTake(state_lock, portMAX_DELAY);
    int64_t now = esp_timer_get_time();
    upload::State st = uploader ? uploader->state() : upload::EMPTY;
    // Field by field: a whole UploadView as a temporary would sit on the web server's stack.
    v.name[0] = v.serial[0] = v.mk2vsc[0] = v.check_why[0] = 0;
    v.bytes = v.firmware = v.saved = 0;
    v.upload_form = v.checked = v.check_ok = false;
    v.credentials = 0;
    v.protected_changes = v.restart_required = false;
    v.settings = 0;
    v.assistants = v.removes_assistants = v.adds_assistants = false;
    v.assistant_bytes_differ = 0;
    v.assistant_first_differ = -1;
    v.state = upload::Uploader::state_name(st);
    v.busy = uploader && uploader->busy();
    snprintf(v.message, sizeof v.message, "%s", uploader ? uploader->message() : "");
    const vebus::Requester::SettingsRead &r = requester.settings();
    v.read_done = r.done && !r.active && !r.inspection;
    v.read_active = r.active;
    v.read_age_s = r.finished >= 0 ? (now - r.finished) / 1e6f : -1;
    if (st != upload::EMPTY) {
        const rvsc::File &f = uploader->file();
        snprintf(v.name, sizeof v.name, "%s", uploader->name());
        v.bytes = (unsigned)uploader->bytes();
        snprintf(v.serial, sizeof v.serial, "%s", f.serial);
        v.firmware = f.firmware & 0xFFFFFF;
        v.saved = f.saved;
        v.upload_form = f.upload_form;
        v.credentials = f.credentials.count;
        snprintf(v.mk2vsc, sizeof v.mk2vsc, "%s", f.mk2vsc);
        const rvsc::Check *c = nullptr;
        if (st == upload::WRITING) {
            c = &uploader->job_check();
        } else if (v.read_done) {
            c = &uploader->preview(r, (int64_t)time(nullptr));
        }
        if (c) {
            v.checked = true;
            v.check_ok = c->ok;
            snprintf(v.check_why, sizeof v.check_why, "%s", c->why);
            const vebus::Requester::WritePlan &p = c->plan;
            v.settings = c->ok ? p.settings : 0;
            for (int i = 0; i < v.settings; i++) {
                v.id[i] = p.setting_id[i];
                v.from[i] = r.s[p.setting_id[i]].value;
                v.to[i] = p.setting_value[i];
            }
            v.assistants = c->ok && (p.assistants || p.disable_assistants);
            v.protected_changes = p.required_access != 0;
            v.restart_required = p.restart;
            v.removes_assistants = c->removes_assistants;
            v.adds_assistants = c->adds_assistants;
            v.assistant_bytes_differ = c->assistant_bytes_differ;
            v.assistant_first_differ = c->assistant_first_differ;
        }
    }
    vebus::Requester::CtlState cs = requester.control_state(now);
    v.master = cs == vebus::Requester::CtlState::POLLING || cs == vebus::Requester::CtlState::CHECKING ||
               cs == vebus::Requester::CtlState::WRITING;
    v.ctl = requester.control_state_name(now);
    const vebus::Requester::WriteJob &j = requester.write_job();
    v.write_active = j.active;
    v.write_step = vebus::Requester::write_step_name(j.step);
    v.write_progress = vebus::Requester::write_progress(j, requester.write_plan());
    v.settings_written = j.settings_written;
    v.records_written = j.records_written;
    v.write_index = j.index;
    v.access_only = uploader && uploader->access_only();
    v.authorized = j.authorized;
    v.access_requests = j.access_requests;
    v.access_reply = j.access_reply;
    v.config_sends = j.config_sends;
    v.reset_sends = j.reset_sends;
    xSemaphoreGive(state_lock);
}

void listener_state(vebus::State &out, faults::Report *report) {
    xSemaphoreTake(state_lock, portMAX_DELAY);
    out = decoder.state();
    if (report) *report = requester.fault_report();
    xSemaphoreGive(state_lock);
}

void listener_state(vebus::State &out) { listener_state(out, nullptr); }

const warnings::History &listener_warning_history() { return warning_history; }
const warnings::Live &listener_warning_status() { return warning_live; }

static void save_warning_history() {
    if (!warning_history.dirty) return;
    const auto &data = warning_history.data;
    if (prefs.putBytes(WARNING_HISTORY_KEY, &data, sizeof data) == sizeof data) warning_history.dirty = false;
}

static void sample_warning_history() {
    static uint32_t sampled_ms = 0, saved_ms = 0;
    const uint32_t ms = millis();
    if (ms - sampled_ms < 250) return;
    sampled_ms = ms;
    xSemaphoreTake(state_lock, portMAX_DELAY);
    const int64_t now = esp_timer_get_time();
    const auto &state = decoder.state();
    warning_live = warnings::decode(state, now, requester.fault_report());
    warning_live.levels[warnings::AC_NOT_ACCEPTED] =
        input_watch.sample(state, warning_live, now, ac_input.requested, requester.ac_input_wire().actual(now));
    xSemaphoreGive(state_lock);
    const time_t utc = time(nullptr);
    warning_history.observe(warning_live, {utc >= CLOCK_SET_AFTER ? uint32_t(utc) : 0u, uint32_t(now / 1000000), system_boot_id});
    if (warning_history.dirty && ms - saved_ms >= MEMORY_SAVE_MS) {
        saved_ms = ms;
        save_warning_history();
    }
}

// Strict snapshot: in particular do not call watch_charger(), which queues reads.
void listener_diagnostics(diagnostics::Cache &v) {
    xSemaphoreTake(state_lock, portMAX_DELAY);
    v.now = esp_timer_get_time(); v.policy_at = policy_sample_at;
    v.charger = requester.charger_view(v.now);
    v.faults = requester.fault_report();
    if(!v.faults.firmware)v.faults.firmware=requester.ac_input_wire().firmware;
    v.led_code=faults::led_code(decoder.state(),v.now);
    v.external_battery = battery_view(v.now);
    const auto &soc = decoder.state().soc; const auto &sync = requester.soc_sync();
    v.own_soc_valid = fresh(soc,v.now,SOC_FRESH_US); v.own_soc = soc.v;
    v.sync_target = sync.target_soc; v.sync_sent = sync.soc_sent;
    v.sync_age_s = sync.last_soc<0?-1:float(v.now-sync.last_soc)/1e6f;
    v.sync_verified = v.external_battery.sync_verified;
    v.max_charge_w=ess_settings.max_charge_w; v.max_discharge_w=ess_settings.max_discharge_w;
    v.max_import_w=ess_settings.max_import_w; v.max_feed_in_w=ess_settings.max_feed_in_w;
    v.safety_max_w=ess_settings.safety_max_w; v.minimum_soc=ess_settings.min_soc;
    v.frame_errors=stats.frame_err; v.parity_errors=stats.parity_err;
    v.fifo_overflows=stats.fifo_ovf; v.buffer_full=stats.buf_full; v.oversize=stats.oversize;
    xSemaphoreGive(state_lock);
}

bool listener_fault_report_read(char *why,size_t cap) {
    xSemaphoreTake(state_lock,portMAX_DELAY);
    bool ok=requester.read_fault_report(esp_timer_get_time(),why,cap);
    xSemaphoreGive(state_lock);
    return ok;
}

size_t listener_tx_json(char *out, size_t cap) {
    xSemaphoreTake(state_lock, portMAX_DELAY);
    size_t n = requester.to_json(esp_timer_get_time(), out, cap);
    xSemaphoreGive(state_lock);
    return n;
}

void setup() {
    system_boot_id = esp_random();
    ac_input.revision=system_boot_id;
    pv_disable.revision=system_boot_id;
    // The transceiver's driver stays off unless one of our frames is going out.
    gpio_set_level((gpio_num_t)DE_PIN, 0);
    gpio_config_t de = {};
    de.pin_bit_mask = 1ULL << DE_PIN;
    de.mode = GPIO_MODE_OUTPUT;
    ESP_ERROR_CHECK(gpio_config(&de));
    gpio_set_level((gpio_num_t)DE_PIN, 0);

    state_lock = xSemaphoreCreateMutex();
    commands = xQueueCreate(4, sizeof(Command));
    prefs.begin("ess", false);
    load_settings();
    warning_history.revision = system_boot_id;
    warnings::Stored saved_warnings;
    if (prefs.getBytesLength(WARNING_HISTORY_KEY) == sizeof saved_warnings &&
        prefs.getBytes(WARNING_HISTORY_KEY, &saved_warnings, sizeof saved_warnings) == sizeof saved_warnings) {
        warning_history.restore(saved_warnings);
    }
    setenv("TZ", ess_settings.tz, 1);
    tzset();
    Serial0.setTxBufferSize(16384);  // frames queue here instead of stalling the bus task
    Serial0.begin(OUT_BAUD);
    Serial.setTxBufferSize(16384);
    Serial.setTxTimeoutMs(0);  // never block when nothing reads the USB port
    Serial.begin(115200);

    uart_config_t cfg = {};
    cfg.baud_rate = BAUD;
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_APB;  // 80 MHz / 320 = 250,000 baud exactly
    ESP_ERROR_CHECK(uart_driver_install(BUS, 8192, 0, 64, &events, 0));
    ESP_ERROR_CHECK(uart_param_config(BUS, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(BUS, TX_PIN, RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    if (VEBUS_INVERT_RX) ESP_ERROR_CHECK(uart_set_line_inverse(BUS, UART_SIGNAL_RXD_INV | UART_SIGNAL_TXD_INV));
    ESP_ERROR_CHECK(uart_set_rx_timeout(BUS, RX_TIMEOUT_BYTES));
    ESP_ERROR_CHECK(uart_set_rx_full_threshold(BUS, 64));
    ESP_ERROR_CHECK(uart_set_tx_idle_num(BUS, 0));  // the rest of a frame follows the address pause at once

    say("# vebus_listener: UART1 RX=GPIO%d TX=GPIO%d DE=GPIO%d, %d baud 8N1%s; controller %s\n", RX_PIN, TX_PIN,
        DE_PIN, BAUD, VEBUS_INVERT_RX ? ", RX and TX inverted" : "",
        VEBUS_CONTROL_AT_BOOT ? "on: polls when no other master is heard, runs ESS only when set to"
                              : "off until \"ctl on\"");
    int schedules = 0;
    for (const ess::Schedule &w : ess_settings.schedule) schedules += w.enabled;
    say("# ess settings: %s, %s, minimum SOC %.0f %%, grid setpoint %+d W (+ = import), safety cap %d W, %d of %d "
        "charge schedules on, time zone %s\n",
        ess_settings.enabled ? "run ESS from here" : "ESS from here off", ess::mode_name(ess_settings.mode),
        ess_settings.min_soc, ess_settings.grid_setpoint_w, ess_settings.safety_max_w, schedules, ess::SCHEDULES,
        ess_settings.tz);
    requester.monitor_alarm_status(true);
    requester.control(VEBUS_CONTROL_AT_BOOT);
    firmware_ota::begin();
    // Wi-Fi runs on core 0, so the bus task goes on core 1 with UART1's interrupt (set up here).
    xTaskCreatePinnedToCore(bus_task, "vebus", 8192, nullptr, 20, nullptr, 1);
    web::begin();
    configTzTime(ess_settings.tz, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
    mqtt_link::begin(mqtt_settings);
}

void loop() {
    read_commands();
    sample_warning_history();
    web::loop();
    mqtt_link::loop();
    static uint32_t last_voltage_save = 0;
    if (voltage_hold_dirty && millis() - last_voltage_save >= 1000) {
        xSemaphoreTake(state_lock, portMAX_DELAY);
        const uint32_t hold = ess_settings.voltage_hold_cv | (policy.voltage_latched() ? VOLTAGE_HOLD_LATCHED : 0);
        voltage_hold_dirty = false;
        xSemaphoreGive(state_lock);
        if (prefs.putUInt("volthold", hold) != sizeof hold) voltage_hold_dirty = true;
        last_voltage_save = millis();
    }
    if (ess_dirty && millis() - ess_saved_ms >= ESS_SAVE_MS) {
        ess::Settings s;
        xSemaphoreTake(state_lock, portMAX_DELAY);
        s = ess_settings;
        xSemaphoreGive(state_lock);
        save_settings(s);
        ess_dirty = false;
        ess_saved_ms = millis();
    }
    static uint32_t saved_ms = 0;
    if (memory_dirty && millis() - saved_ms >= MEMORY_SAVE_MS) {
        ess::Memory m;
        xSemaphoreTake(state_lock, portMAX_DELAY);
        m = policy.memory;
        memory_dirty = false;
        xSemaphoreGive(state_lock);
        prefs.putBytes("memory", &m, sizeof m);
        saved_ms = millis();
    }
    if (system_restart_at >= 0 && esp_timer_get_time() >= system_restart_at) {
        xSemaphoreTake(state_lock, portMAX_DELAY);
        const int64_t now = esp_timer_get_time();
        const bool busy = system_restart_wifi ? !wifi_restart_safe_locked(now) :
                          system_restart_firmware ? !firmware_safe_locked(now) : manual_restart_busy_locked(now);
        ess::Settings settings = ess_settings;
        ess::Memory memory = policy.memory;
        xSemaphoreGive(state_lock);
        const char *cancelled = nullptr;
        bool save_pending = voltage_hold_dirty;
        if (busy || save_pending ||
            (system_restart_shutdown && !web::recovery_restart_ready())) {
            cancelled = system_restart_wifi ? "Wi-Fi restart deferred: grid or controller busy"
                                            : "Restart cancelled: grid, operations or settings storage are not ready";
        } else if (!system_restart_shutdown) {
            // Let the browser receive the response and the AP see the station leave.
            // A plain ESP.restart() can leave an unusable association on this AP.
            web::begin_restart_shutdown();
            system_restart_shutdown = true;
            system_restart_at = esp_timer_get_time() + int64_t(wifi_recovery::RESTART_WAIT_MS) * 1000;
        } else if (prefs.putBytes("settings", &settings, ESS_SETTINGS_BYTES) != ESS_SETTINGS_BYTES ||
                   prefs.putBytes("memory", &memory, sizeof memory) != sizeof memory) {
            cancelled = "Restart cancelled: settings storage failed";
        } else {
            save_warning_history();
            if (system_restart_firmware && !firmware_ota::activate()) {
                cancelled = "Firmware restart cancelled. Release BOOT and check the OTA slot.";
            } else {
                if (system_restart_wifi) web::mark_recovery_restart();
                ESP.restart();
            }
        }
        if (cancelled) {
            system_restart_at = -1;
            system_restart_message = cancelled;
            if (system_restart_shutdown) web::cancel_recovery_restart();
            if (system_restart_firmware) firmware_ota::cancel_restart(cancelled);
            system_restart_wifi = system_restart_firmware = false;
            system_restart_shutdown = false;
        }
    }
    delay(2);
}
