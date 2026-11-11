// Wi-Fi and the pages: joins the saved network (or the provisioned fallback) and serves on port 80
// (http://brain-zero.local/):
//   /                the dashboard, polling /api/state, the decoded state as JSON
//   /ess             the ESS settings (Venus's ESS menu), reading GET /api/ess and saving with
//                    POST /api/ess, form fields as in settings_from_form() in web.cpp
//   /inverter        Venus's operating controls: GET /api/inverter, POST with exactly one control
//                    Stored charger parameters: GET/POST /api/charger, POST /api/charger/read
//   /settings        Wi-Fi, Remote Console access, and language
//   /settings/mqtt   the MQTT broker and Home Assistant discovery, GET and POST /api/mqtt
//   /multi           the Multi's settings: POST /api/settings/read reads them, with ?inspect=1
//                    for setup. GET /api/settings gives the readback, with JSON, CSV and full backups,
//                    GET /api/settings.rvsc. A file is written to the Multi (upload.h) with POST
//                    /api/upload (the file), GET /api/upload (what it would change, the job's progress),
//                    POST /api/upload/write, /api/upload/cancel and /api/upload/clear.
// The ESS settings decide what the controller writes (ess_policy.h, requester.h), and only while no
// other master is on the bus. Web and API access can be protected with the Remote Console password.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ess_policy.h"
#include "ess_lease.h"
#include "external_battery.h"
#include "inverter_controls.h"

class String;
struct cJSON;

namespace web {

struct InverterSetupView {
    const char *status = "not_read";
    const char *model = "";
    uint8_t nominal_voltage = 0;
    uint8_t ess_setpoint_ram = 0;
    bool can_upload = false;
};

// Where writing a settings file stands, from main.cpp (upload.h), for the Multi settings page.
struct UploadView {
    const char *state = "empty";  // upload::Uploader::state_name()
    bool busy = false;
    char message[200] = "";
    char name[48] = "";           // the file
    unsigned bytes = 0;
    char serial[16] = "";
    uint32_t firmware = 0, saved = 0;
    bool upload_form = false;
    unsigned credentials = 0;
    bool protected_changes = false, restart_required = false;
    char mk2vsc[8] = "";
    bool read_done = false, read_active = false;  // the Multi's latest read, which it is checked against
    float read_age_s = -1;
    bool checked = false, check_ok = false;
    char check_why[120] = "";
    int settings = 0;             // the settings that differ: id, the Multi's value, the file's
    uint8_t id[192];
    uint16_t from[192], to[192];
    bool assistants = false, removes_assistants = false, adds_assistants = false;
    int assistant_bytes_differ = 0, assistant_first_differ = -1;
    bool master = false;          // the ESP32 is the only master on the bus
    const char *ctl = "off";
    bool write_active = false;    // the write job (requester.h)
    const char *write_step = "";
    int write_progress = 0, settings_written = 0, records_written = 0, write_index = 0;
    bool access_only = false;
    unsigned authorized = 0, access_requests = 0, config_sends = 0, reset_sends = 0;
    int access_reply = -1;
};

// What the ESS settings make the ESP32 do, from main.cpp, for the pages.
struct EssView {
    inverter::View inverter;
    ess::Settings settings;
    ess::Decision decision;
    ess::Memory memory;
    const char *ctl = "off";  // the controller's state (requester.h)
    int multi_flags = -1;     // the Multi's flag word as last read
    bool soc_valid = false;
    float soc = 0;
    bool voltage_valid = false, voltage_hold_confirmed = false;
    float voltage = 0;
    bool clock_valid = false;
    bool detect_time_zone = false;
    const char *time_zone = "";
    char clock[40] = "";      // local time
};

void begin();                          // from setup()
void loop();                           // often, from the Arduino loop: serves requests
// Broker requests use the same controls as HTTP and return a shared reply buffer.
// Call only from the Arduino loop and consume the reply before the next web or MQTT call.
const char *controller_request(const char *name, bool write, const cJSON *fields);
bool local_host(const String &host);    // permitted controller names and station IP, without port
void reconnect();                      // cycle the station from the Arduino loop, leaving VE.Bus running
bool reset_wifi();                     // physical serial console: restore the provisioned Wi-Fi network
bool reset_password();                 // physical serial console: remove the Remote Console password and API token
void mark_recovery_restart();           // retain the one-restart limit through a controller restart
void begin_restart_shutdown();         // leave the AP before a scheduled restart
bool recovery_restart_ready();         // the Wi-Fi radio has been off for the full hold period
void cancel_recovery_restart();         // let the station rejoin if the safety checks defer the restart
size_t describe(char *out, size_t cap);  // "wifi=..." for the status line

}  // namespace web

bool listener_ess_set(const ess::Settings &settings, char *why, size_t cap, bool save_now,
                      bool legacy_zone = false);
void listener_battery_input(external_battery::View &out);
bool listener_battery_input_enabled(bool value, char *why, size_t cap);
void listener_battery_input_connection(bool connected);
bool listener_battery_input_message(const char *payload, size_t size, bool retained,
                                    int64_t queued_at, const char **why);
void listener_ess_lease(ess_lease::View &out);
const char *listener_ess_lease_set(const char *id, uint32_t ttl_s, int charge_w);
const char *listener_ess_lease_clear(const char *id);
