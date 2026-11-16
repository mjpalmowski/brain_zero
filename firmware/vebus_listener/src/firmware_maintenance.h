#pragma once
#include "firmware_release.h"
#include <stddef.h>
#include <stdint.h>

namespace firmware_maintenance {
enum class Action : uint8_t { NONE, CHECK, STAGE };
enum class Phase : uint8_t { NORMAL, RESTART_PENDING, WAITING, CHECKING, AVAILABLE, DOWNLOADING, STAGED, RESUMING };
enum class Job : uint8_t { IDLE, CHECKING, AVAILABLE, CURRENT, DOWNLOADING, STAGED, ERROR };
constexpr uint32_t IDLE_MS = 300000;
struct Record {
    uint32_t magic, crc, source_offset, source_version;
    struct StoredBuild {
        char tag[24];
        uint32_t version, size;
        uint8_t sha256[32], preview, reserved[3];
    } build;
    Action action;
    uint8_t reserved[3];
};
struct Status {
    const char *phase;
    bool pending, active;
    uint32_t remaining_s;
    const char *message;
};
struct Poll {
    uint32_t now;
    bool network_ready, worker_idle;
    const char *not_ready;
    Job job;
};
struct Hooks {
    const char *(*check)();
    const char *(*stage)(const firmware_release::Build &);
    void (*cancel)();
    bool (*resume_ble)();
};
// Portable state machine; only the Record lives in retained RTC memory.
class State {
  public:
    const char *prepare(Record &, Action, const firmware_release::Build *, uint32_t offset, uint32_t version);
    bool boot(Record &, bool software_reset, uint32_t offset, uint32_t version, uint32_t now);
    void abort_request(Record &);
    void cancel();
    void discarded(uint32_t now);
    void activity(uint32_t now);
    void poll(Record &, const Poll &, const Hooks &);
    bool active() const;
    bool pending() const { return phase_ == Phase::RESTART_PENDING; }
    Status status(uint32_t now) const;
  private:
    Phase phase_ = Phase::NORMAL;
    uint32_t idle_at_ = 0;
    bool cancelled_ = false;
    bool cancel_sent_ = false;
    const char *message_ = "";
};
State &state();
Record &record();
Status status(uint32_t now);
}
