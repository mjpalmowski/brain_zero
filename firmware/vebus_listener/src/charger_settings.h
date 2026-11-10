// Persistent VEConfigure settings for verified single-unit inverter profiles.
// The bus job lives in Requester; these types, validation and JSON are plain C++.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace charger {
constexpr int COUNT = 3;
enum class Profile : uint8_t { CHARGER, ASSIST };
constexpr uint16_t ASSIST_MASK = 1u << 5; // MK2 protocol 3.14, setting 0 / Flags[5]
int count(Profile profile);
enum Index : uint8_t { CURRENT, ABSORPTION, FLOAT };
enum class Stage : uint8_t { IDLE, READING, WRITING, VERIFYING, DONE, FAILED };
struct Parameter {
    bool info = false, known = false;
    int16_t scale = 0, offset = 0;
    uint16_t raw = 0, minimum = 0, maximum = 0;
    uint8_t access = 0;
    int64_t read_at = -1;
};
struct Change {
    uint16_t raw[COUNT] = {}, expected[COUNT] = {};
};
struct Status {
    Profile profile = Profile::CHARGER;
    Stage stage = Stage::IDLE;
    uint8_t nominal_voltage = 0;
    uint16_t product_id = 0; // Venus product ID, packed BCD
    uint32_t firmware = 0;
    Parameter parameter[COUNT];
    bool saving = false, acknowledged[COUNT] = {}, verified[COUNT] = {};
    uint16_t requested[COUNT] = {};
    int changes = 0, writes = 0, confirmations = 0;
    int64_t started = -1, finished = -1;
    char message[160] = "Waiting to read charger settings";
    bool active() const { return stage == Stage::READING || stage == Stage::WRITING || stage == Stage::VERIFYING; }
};
struct View {
    Status status;
    bool available = false;
    int64_t now = 0;
    char reason[128] = "Waiting for charger settings";
};
uint8_t setting_id(int index, Profile profile = Profile::CHARGER);
const char *key(int index, Profile profile = Profile::CHARGER);
const char *stage_name(Stage stage);
bool schema_ok(int index, const Parameter &p, Profile profile, uint8_t nominal_voltage = 0);
// Parse the form's encoding. The connected device's descriptor and voltage class
// establish the range in plan().
bool parse(const char *current, const char *absorption, const char *floating, const char *base,
           Change &out, char *why, size_t cap);
bool parse_assist(const char *enabled, const char *boost, const char *base, Change &out, char *why, size_t cap);
// Validates the fresh device values/schema and builds an order that preserves float <= absorption
// between individual writes. Returns -1 on error, otherwise 0..3 changes.
int plan(const Parameter *p, const Change &change, uint8_t *order, char *why, size_t cap,
         Profile profile, uint8_t nominal_voltage = 0);
size_t to_json(const View &v, char *out, size_t cap);
} // namespace charger
