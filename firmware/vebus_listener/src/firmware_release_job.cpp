#include "firmware_release_job.h"

#include <Arduino.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

namespace firmware_release_job {
namespace {

enum class Phase { IDLE, CHECKING, AVAILABLE, CURRENT, DOWNLOADING, STAGED, ERROR };

constexpr char UNSAFE[] = "Firmware download stopped because the inverter is no longer ready.";
constexpr char BUSY[] = "A GitHub firmware operation is already in progress.";
constexpr uint32_t SAFETY_POLL_MS = 100;
constexpr uint32_t WORKER_STACK_BYTES = 12288;

Phase phase = Phase::IDLE;
firmware_release::Build selected;
firmware_ota::Status before_download = {};
const char *message = "Check GitHub for a newer firmware build.";
const char *blocked_by = "";
uint32_t last_safety_poll = 0;
void (*release_reservation)() = nullptr;

// The loop writes inputs before creating the task. The task publishes its
// result once through completed, and no longer touches these fields afterward.
struct Work {
    bool download;
    uint32_t running;
    size_t capacity;
    firmware_release::Build build;
    const char *error;
};
Work work = {};
std::atomic<bool> completed{false}, cancelled{false};
std::atomic<size_t> received{0};

void release_download_reservation() {
    auto release = release_reservation;
    release_reservation = nullptr;  // clear ownership before invoking the callback
    if (release) release();
}

const char *receive(const uint8_t *data, size_t bytes, void *) {
    if (cancelled.load()) return UNSAFE;
    if (const char *error = firmware_ota::write(data, bytes)) return error;
    received.fetch_add(bytes);
    return nullptr;
}

void run(void *) {
    work.error = work.download ? firmware_release::download(work.build, receive, nullptr)
                               : firmware_release::check(work.running, work.capacity, work.build);
    completed.store(true, std::memory_order_release);
    vTaskDelete(nullptr);
}

const char *launch(Phase next) {
    completed.store(false);
    cancelled.store(false);
    received.store(0);
    blocked_by = "";
    if (xTaskCreatePinnedToCore(run, "firmware-https", WORKER_STACK_BYTES, nullptr, 1, nullptr, 0) != pdPASS) {
        phase = Phase::ERROR;
        message = "Not enough memory to start the GitHub firmware operation.";
        return message;
    }
    phase = next;
    return nullptr;
}

}  // namespace

bool busy() { return phase == Phase::CHECKING || phase == Phase::DOWNLOADING; }

Status status() {
    static const char *const names[] = {"idle", "checking", "available", "current", "downloading", "staged", "error"};
    return {names[static_cast<unsigned>(phase)], selected, received.load(), message,
            phase == Phase::AVAILABLE, blocked_by};
}

firmware_ota::Status ota_status() {
    return phase == Phase::DOWNLOADING ? before_download : firmware_ota::status();
}

const char *check(uint32_t running, size_t capacity) {
    if (busy()) return BUSY;
    selected = {};
    work = {false, running, capacity, {}, nullptr};
    message = "Checking GitHub Releases…";
    return launch(Phase::CHECKING);
}

const char *stage(const char *tag, const char *(*reserve)(), void (*release)()) {
    if (busy()) return BUSY;
    if (phase != Phase::AVAILABLE || !tag || strcmp(tag, selected.tag))
        return "Check GitHub Releases again before downloading.";
    if (!reserve || !release) return "Firmware download reservation is unavailable.";
    if (const char *error = reserve()) return error;
    release_reservation = release;
    before_download = firmware_ota::status();
    if (const char *error = firmware_ota::start(selected.size)) {
        release_download_reservation();
        return error;
    }
    work = {true, 0, 0, selected, nullptr};
    last_safety_poll = millis();
    message = "Downloading firmware from brain-zero.com…";
    if (const char *error = launch(Phase::DOWNLOADING)) {
        firmware_ota::discard(error);
        release_download_reservation();
        return error;
    }
    return nullptr;
}

void poll(const char *(*not_ready)()) {
    if (!busy()) return;
    const bool downloading = phase == Phase::DOWNLOADING;
    if (downloading && millis() - last_safety_poll >= SAFETY_POLL_MS) {
        last_safety_poll = millis();
        if (!cancelled.load()) {
            if (const char *reason = not_ready()) {
                blocked_by = reason;
                cancelled.store(true);
            }
        }
    }
    if (!completed.load(std::memory_order_acquire)) return;

    const char *error = work.error;
    if (downloading) {
        if (!cancelled.load()) {
            if (const char *reason = not_ready()) {
                blocked_by = reason;
                cancelled.store(true);
            }
        }
        if (cancelled.load()) error = UNSAFE;
        if (!error) error = firmware_ota::finish();
        if (error) firmware_ota::discard(error);
        release_download_reservation();
    }
    if (error) {
        phase = Phase::ERROR;
        message = error;
    } else if (downloading) {
        phase = Phase::STAGED;
        message = "Firmware verified and staged.";
    } else if (work.build.tag[0]) {
        selected = work.build;
        phase = Phase::AVAILABLE;
        message = "A newer build is available.";
    } else {
        phase = Phase::CURRENT;
        message = "GitHub has no newer build for this controller.";
    }
}

void discarded() {
    if (phase == Phase::STAGED) {
        phase = Phase::AVAILABLE;
        received.store(0);
        message = "A newer build is available.";
    }
}

}  // namespace firmware_release_job
