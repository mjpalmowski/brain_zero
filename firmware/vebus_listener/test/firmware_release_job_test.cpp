// Run the production job with deterministic task, OTA, reservation and HTTPS seams.
#include "firmware_release_job.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include "release_stubs/freertos/task.h"

namespace {
void (*task)(void *) = nullptr;
bool offer = false, failed_check = false, safe_now = true, fail_task = false;
bool failed_download = false, failed_write = false, failed_start = false, failed_finish = false;
bool reserved = false, automatic_read_busy = false, cross_refresh_deadline = false;
bool inject_temporary_unsafe = false;
bool suspended = false, defer_suspend = false;
int deleted_workers = 0;
uint32_t now_ms = 1000;
int checks = 0, starts = 0, downloads = 0, writes = 0, finishes = 0, discards = 0;
int reserve_calls = 0, release_calls = 0, automatic_reads = 0;
constexpr char tag[] = "firmware-20261111.7";
constexpr char unsafe[] = "Grid is no longer confirmed connected.";

const char *problem() {
    if (!safe_now) return unsafe;
    return automatic_read_busy ? "A settings read is in progress." : nullptr;
}
const char *reserve() {
    ++reserve_calls;
    if (reserved) return "Another download owns the reservation.";
    if (const char *why = problem()) return why;
    reserved = true; return nullptr;
}
void release() {
    assert(reserved); // A failed acquisition must never release another owner.
    reserved = false; ++release_calls;
}
// A periodic read makes the old strict idle check false. The real Requester
// collision is tested independently; this seam tests ownership over that boundary.
void automatic_refresh_tick() {
    if (!reserved) { automatic_read_busy = true; ++automatic_reads; }
}
const char *stage() { return firmware_release_job::stage(tag, reserve, release); }
void complete() { assert(task); auto run = task; task = nullptr; run(nullptr); }
void poll() { firmware_release_job::poll(problem); }
void check() {
    const int before = checks, before_reserve = reserve_calls, before_release = release_calls;
    assert(!firmware_release_job::check(2026111106, 3342336));
    assert(firmware_release_job::busy() && !firmware_release_job::status().can_stage);
    assert(firmware_release_job::check(2026111106, 3342336)); assert(stage());
    complete(); poll();
    assert(checks == before + 1 && reserve_calls == before_reserve && release_calls == before_release);
}
void available() {
    offer = true; check();
    assert(!std::strcmp(firmware_release_job::status().phase, "available"));
    assert(firmware_release_job::status().can_stage);
}
}
uint32_t millis() { return now_ms; }
BaseType_t xTaskCreatePinnedToCore(void (*run)(void *), const char *, unsigned, void *, unsigned, void *handle, int) {
    if (fail_task) return 0;
    assert(!task); task = run; suspended = false;
    *static_cast<TaskHandle_t *>(handle) = reinterpret_cast<void *>(1); return pdPASS;
}
void vTaskDelete(void *handle) { assert(handle && suspended); ++deleted_workers; suspended = false; }
void vTaskSuspend(void *) { suspended = !defer_suspend; }
eTaskState eTaskGetState(TaskHandle_t handle) { assert(handle); return suspended ? eSuspended : eRunning; }
namespace firmware_ota {
Status status() { return {false, false, 3342336, 0, 0, "app0", "app1", "token", "Ready", Action::NONE}; }
const char *start(size_t bytes) {
    assert(reserved && bytes == 1024); ++starts;
    return failed_start ? "Start failed" : nullptr;
}
const char *write(const uint8_t *, size_t size) {
    assert(reserved && size == 256); ++writes;
    return failed_write ? "Write failed" : nullptr;
}
const char *finish() {
    assert(reserved); ++finishes;
    return failed_finish ? "Finish failed" : nullptr;
}
void discard(const char *error) { assert(reserved && error && *error); ++discards; }
}
namespace firmware_release {
const char *check(uint32_t running, size_t capacity, Build &out) {
    assert(running == 2026111106 && capacity == 3342336); ++checks; out = {};
    if (failed_check) return "Secure check failed";
    if (offer) { std::strcpy(out.tag, tag); out.version = 2026111107; out.size = 1024; std::memset(out.sha256, 0x51, sizeof out.sha256); }
    return nullptr;
}
const char *download(const Build &build, Sink sink, void *context) {
    assert(reserved && !std::strcmp(build.tag, tag) && build.size == 1024); ++downloads;
    if (failed_download) return "Download failed";
    const uint8_t bytes[256] = {};
    for (int i = 0; i < 4; ++i) {
        if (cross_refresh_deadline) {
            now_ms += 15001; automatic_refresh_tick(); poll();
        }
        if (inject_temporary_unsafe && i == 1) {
            safe_now = false; now_ms += 101; poll();
            safe_now = true; // Recovery cannot undo a known unsafe sample.
        }
        if (const char *error = sink(bytes, sizeof bytes, context)) return error;
    }
    return nullptr;
}
}
int main() {
    using namespace firmware_release_job;
    check();
    assert(!std::strcmp(status().phase, "current") && !status().can_stage);
    assert(stage() && !task && !starts && !downloads && !writes && !finishes && !reserve_calls);
    available();
    assert(!std::strcmp(status().build.tag, tag));
    assert(firmware_release_job::stage("firmware-20261111.8", reserve, release) && !reserve_calls);
    assert(firmware_release_job::stage(tag, nullptr, release) && !reserve_calls);
    assert(firmware_release_job::stage(tag, reserve, nullptr) && !reserve_calls);

    // Strict readiness is checked by the reservation callback before OTA start.
    safe_now = false;
    const int entry_releases = release_calls;
    assert(stage() && !reserved && !starts && release_calls == entry_releases);
    safe_now = true;
    reserved = true; // A separate owner must survive our failed acquisition.
    assert(stage() && reserved && !starts && release_calls == entry_releases);
    reserved = false;

    // Cross a complete 60-second refresh interval while holding the reservation.
    automatic_refresh_tick(); assert(automatic_read_busy && problem());
    automatic_read_busy = false;
    const int prior_reads = automatic_reads;
    cross_refresh_deadline = true;
    assert(!stage() && reserved && starts == 1);
    assert(busy() && !status().can_stage && !std::strcmp(ota_status().slot, "app0"));
    const int held_reserves = reserve_calls;
    assert(stage() && firmware_release_job::check(0, 0) && reserve_calls == held_reserves);
    complete(); assert(reserved && writes == 4 && !finishes);
    poll();
    assert(!reserved && automatic_reads == prior_reads && downloads == 1 && finishes == 1 && !discards);
    assert(status().received == 1024 && !std::strcmp(status().phase, "staged"));
    const int successful_releases = release_calls;
    discarded();
    assert(!std::strcmp(status().phase, "available") && status().received == 0 && release_calls == successful_releases);
    cross_refresh_deadline = false;
    automatic_refresh_tick(); assert(automatic_reads == prior_reads + 1); automatic_read_busy = false;

    failed_start = true;
    int old_releases = release_calls;
    assert(stage() && !reserved && !task && release_calls == old_releases + 1);
    failed_start = false;
    // Every error discards while still holding its reservation, then releases it once.
    for (int failure = 0; failure < 4; ++failure) {
        available();
        failed_download = failure == 0; failed_write = failure == 1;
        failed_finish = failure == 2; fail_task = failure == 3;
        const int old_finishes = finishes, old_discards = discards;
        old_releases = release_calls;
        const char *error = stage();
        if (fail_task) assert(error && !task);
        else { assert(!error && reserved); complete(); assert(reserved); poll(); }
        assert(!reserved && release_calls == old_releases + 1);
        assert(!std::strcmp(status().phase, "error") && !status().can_stage);
        assert(discards == old_discards + 1 && finishes == old_finishes + (failed_finish ? 1 : 0));
        failed_download = failed_write = failed_finish = fail_task = false;
    }
    // Unsafe remains a hard cancellation, even if it recovers before the next chunk.
    available(); assert(!stage());
    const int before_writes = writes, before_finishes = finishes;
    old_releases = release_calls;
    safe_now = false; now_ms += 101; poll(); safe_now = true;
    automatic_read_busy = true; now_ms += 101; poll();
    assert(!std::strcmp(status().blocked_by, unsafe)); // A later predicate cannot replace the first cause.
    automatic_read_busy = false;
    assert(reserved); complete(); poll();
    assert(!reserved && release_calls == old_releases + 1 && writes == before_writes && finishes == before_finishes);
    assert(!std::strcmp(status().phase, "error") && !std::strcmp(status().blocked_by, unsafe));

    // Cancel between successful chunks, not just before byte zero.
    available(); inject_temporary_unsafe = true; assert(!stage());
    const int partial_before = writes; old_releases = release_calls;
    complete(); poll(); inject_temporary_unsafe = false;
    assert(!reserved && writes == partial_before + 1 && status().received == 256 && finishes == before_finishes);
    assert(release_calls == old_releases + 1 && !std::strcmp(status().blocked_by, unsafe));

    // A failed final safety check cannot turn a complete download into a staged image.
    available(); assert(!stage()); complete();
    assert(reserved); safe_now = false; old_releases = release_calls; poll(); safe_now = true;
    assert(!reserved && release_calls == old_releases + 1 && finishes == before_finishes && !std::strcmp(status().phase, "error"));
    assert(!std::strcmp(status().blocked_by, unsafe));

    failed_check = true; check();
    assert(!std::strcmp(status().phase, "error") && !status().can_stage && !status().build.tag[0]);
    assert(stage() && !task && !reserved); failed_check = false;
    fail_task = true; old_releases = release_calls;
    assert(firmware_release_job::check(2026111106, 3342336));
    assert(!busy() && !status().can_stage && !task && !reserved && release_calls == old_releases);
    fail_task = false;

    // A published result is not permission to resume BLE while the worker can
    // still run or its stack remains allocated.
    offer = true; defer_suspend = true;
    assert(!firmware_release_job::check(2026111106, 3342336));
    complete(); const int deleted_before = deleted_workers;
    poll(); assert(busy() && !worker_idle() && deleted_workers == deleted_before);
    suspended = true; poll(); defer_suspend = false;
    assert(!busy() && worker_idle() && deleted_workers == deleted_before + 1);

    firmware_release::Build selected;
    assert(!selection(tag, selected));
    assert(selection("firmware-20261111.8", selected));
    assert(selected.version == 2026111107 && selected.size == 1024 && selected.sha256[0] == 0x51);
    const int before_invalid = reserve_calls;
    auto invalid = selected; ++invalid.version;
    assert(resume_stage(invalid, reserve, release));
    invalid = selected; std::memset(invalid.sha256, 0, sizeof invalid.sha256);
    assert(resume_stage(invalid, reserve, release));
    invalid = selected; invalid.size = 4000000;
    assert(resume_stage(invalid, reserve, release));
    assert(reserve_calls == before_invalid);
    safe_now = false;
    assert(resume_stage(selected, reserve, release) && !reserved);
    safe_now = true;
    assert(!resume_stage(selected, reserve, release));
    complete(); poll(); assert(worker_idle() && !std::strcmp(status().phase, "staged"));
    discarded();

    // User cancellation also waits for the worker, releases the exact owned
    // reservation, and never stages the cancelled transfer.
    assert(!stage()); const int before_cancel_writes = writes;
    firmware_release_job::cancel(); assert(!worker_idle());
    complete(); poll();
    assert(worker_idle() && !reserved && writes == before_cancel_writes);
    assert(!std::strcmp(status().message, "Firmware operation cancelled.") && !std::strcmp(status().phase, "error"));
    assert(!firmware_release_job::check(2026111106, 3342336));
    firmware_release_job::cancel(); complete(); poll();
    assert(worker_idle() && !std::strcmp(status().message, "Firmware operation cancelled."));
    puts("Release job reservation, strict safety, exact replay, cancellation and suspended-worker stack cleanup tests passed.");
}
