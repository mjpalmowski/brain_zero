#include "firmware_maintenance.h"
#include "firmware_ota.h"
#include <string.h>
#include <type_traits>
#if defined(ARDUINO_ARCH_ESP32)
#include <esp_attr.h>
#endif

namespace firmware_maintenance {
static_assert(std::is_trivial<Record>::value, "RTC record must have no boot-time constructor");
namespace {
constexpr uint32_t MAGIC = 0x425a4d31;
constexpr char PAUSED[] = "Bluetooth battery monitoring is paused for firmware update.";
constexpr char PREPARING[] = "Preparing firmware update.";
constexpr char CANCELLED[] = "Firmware operation cancelled.";
constexpr char EXPIRED[] = "Firmware update mode timed out.";
State current;
#if defined(ARDUINO_ARCH_ESP32)
RTC_NOINIT_ATTR Record retained;
#else
Record retained{};
#endif
uint32_t checksum(const Record &r) {
    uint32_t crc = UINT32_MAX;
    const auto *bytes = reinterpret_cast<const uint8_t *>(&r);
    for (size_t i = offsetof(Record, source_offset); i < sizeof r; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return ~crc;
}
template <typename Build> bool valid_build(const Build &b) {
    bool digest = false;
    for (uint8_t v : b.sha256) digest |= v != 0;
    return memchr(b.tag, 0, sizeof b.tag) && b.version && digest &&
           firmware_release::tag_version(b.tag) == b.version && b.size >= firmware_ota::PREFIX_BYTES;
}
}
State &state() { return current; }
Record &record() { return retained; }
Status status(uint32_t now) { return current.status(now); }

const char *State::prepare(Record &r, Action action, const firmware_release::Build *build,
                           uint32_t offset, uint32_t version) {
    if (active() || pending()) return "A firmware restart is already queued.";
    if (!offset || !version || (action != Action::CHECK && action != Action::STAGE) ||
        (action == Action::STAGE && (!build || !valid_build(*build))))
        return "Invalid GitHub firmware selection.";
    r = {};
    r.source_offset = offset; r.source_version = version; r.action = action;
    if (build) {
        memcpy(r.build.tag, build->tag, sizeof r.build.tag);
        r.build.version = build->version; r.build.size = build->size;
        memcpy(r.build.sha256, build->sha256, sizeof r.build.sha256);
        r.build.preview = build->preview;
    }
    r.crc = checksum(r);
    r.magic = MAGIC;
    phase_ = Phase::RESTART_PENDING; cancelled_ = cancel_sent_ = false;
    message_ = PREPARING;
    return nullptr;
}

bool State::boot(Record &r, bool software_reset, uint32_t offset, uint32_t version, uint32_t now) {
    const bool valid = software_reset && offset && version && r.magic == MAGIC && r.crc == checksum(r) &&
                       offset == r.source_offset && version == r.source_version &&
                       (r.action == Action::CHECK || (r.action == Action::STAGE && valid_build(r.build)));
    r.magic = 0;  // Consume before any network, worker or Bluetooth allocation.
    phase_ = valid ? Phase::WAITING : Phase::NORMAL;
    cancelled_ = cancel_sent_ = false; idle_at_ = now;
    message_ = valid ? PREPARING : "";
    if (!valid) r = {};
    return valid;
}
void State::abort_request(Record &r) {
    if (!pending()) return;
    r = {}; phase_ = Phase::NORMAL; cancelled_ = cancel_sent_ = false; message_ = "";
}
bool State::active() const { return phase_ >= Phase::WAITING; }
void State::cancel() { if (active()) { cancelled_ = true; message_ = CANCELLED; } }
void State::discarded(uint32_t now) { if (active()) { idle_at_ = now; cancel(); } }
void State::activity(uint32_t now) { if (active()) idle_at_ = now; }
Status State::status(uint32_t now) const {
    static const char *const names[] = {"normal", "restart_pending", "waiting", "checking", "available", "downloading", "staged", "resuming"};
    const uint32_t elapsed = now - idle_at_;
    return {names[unsigned(phase_)], pending(), active(), active() && elapsed < IDLE_MS
                ? (IDLE_MS - elapsed + 999) / 1000 : 0, message_};
}
void State::poll(Record &r, const Poll &in, const Hooks &hooks) {
    if (!active()) return;
    if (!cancelled_ && uint32_t(in.now - idle_at_) >= IDLE_MS) {
        cancelled_ = true; message_ = EXPIRED;
    }
    if (cancelled_) {
        if (!in.worker_idle && !cancel_sent_) { hooks.cancel(); cancel_sent_ = true; }
        phase_ = Phase::RESUMING;
    }
    if (phase_ == Phase::WAITING && in.network_ready && !in.not_ready && in.worker_idle) {
        firmware_release::Build build;
        memcpy(build.tag, r.build.tag, sizeof build.tag);
        build.version = r.build.version; build.size = r.build.size;
        memcpy(build.sha256, r.build.sha256, sizeof build.sha256); build.preview = r.build.preview;
        const char *error = r.action == Action::CHECK ? hooks.check() : hooks.stage(build);
        if (error) { phase_ = Phase::RESUMING; message_ = error; }
        else { phase_ = r.action == Action::CHECK ? Phase::CHECKING : Phase::DOWNLOADING; idle_at_ = in.now; message_ = PAUSED; }
        return;  // Inputs describe the job before this launch.
    }
    if ((phase_ == Phase::CHECKING || phase_ == Phase::DOWNLOADING) && in.worker_idle) {
        if (in.job == Job::AVAILABLE) { phase_ = Phase::AVAILABLE; idle_at_ = in.now; }
        else if (in.job == Job::STAGED) { phase_ = Phase::STAGED; idle_at_ = in.now; }
        else if (in.job == Job::CURRENT || in.job == Job::ERROR || in.job == Job::IDLE) phase_ = Phase::RESUMING;
    }
    if (phase_ == Phase::AVAILABLE && in.job == Job::DOWNLOADING) {
        phase_ = Phase::DOWNLOADING; idle_at_ = in.now;
    }
    if (phase_ == Phase::AVAILABLE && in.job == Job::CHECKING) {
        phase_ = Phase::CHECKING; idle_at_ = in.now;
    }
    if (phase_ == Phase::AVAILABLE && in.job == Job::STAGED) {
        phase_ = Phase::STAGED; idle_at_ = in.now;
    }
    if (phase_ == Phase::RESUMING && in.worker_idle && hooks.resume_ble()) {
        phase_ = Phase::NORMAL; r = {}; cancelled_ = cancel_sent_ = false;
    }
}
}
