// Venus's user-facing VE.Bus operating controls. Plain C++ for native verification.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "vebus_state.h"

namespace inverter {
enum class Action : uint8_t { NONE, MODE, CURRENT, EQUALIZE, ABSORPTION, FLOAT, RESTART, REDETECT };
enum class Progress : uint8_t { IDLE, QUEUED, SENDING, VERIFYING, CONFIRMED, FAILED };
struct Command { Action action = Action::NONE; int value = 0; }; // mode: wire value 4..7; current: deci-amps
struct Job {
    Command command;
    Progress progress = Progress::IDLE;
    int sends = 0;
    int64_t started = -1, sent_at = -1, next_send = 0;
    char message[128] = "No command sent";
    bool active() const { return progress == Progress::QUEUED || progress == Progress::SENDING || progress == Progress::VERIFYING; }
};
struct View {
    bool available = false, mode_adjustable = false, current_adjustable = false;
    bool can_equalize = false, can_interrupt = false;
    int mode = -1, main_state = -1, charge_state = -1;
    float current = -1, minimum = -1, maximum = -1;
    char reason[128] = "Waiting for the inverter";
    Job job;
};
const char *mode_name(int wire_mode);
const char *charge_name(int state);
const char *progress_name(Progress p);
bool command_key(const char *key);
bool parse(const char *key, const char *value, Command &out, char *why, size_t cap);
View view(const vebus::State &st, const Job &job, bool master, int64_t now);
bool validate(const Command &c, const View &v, char *why, size_t cap);
size_t to_json(const View &v, char *out, size_t cap);
} // namespace inverter
