// Run the real single-channel job with deterministic task, OTA and HTTPS seams.
#include "firmware_release_job.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include "release_stubs/freertos/task.h"

namespace {
void (*task)(void *) = nullptr;
bool offer = false, failed_check = false, safe_now = true, fail_task = false;
bool failed_download = false, failed_write = false, failed_start = false, failed_finish = false;
uint32_t now_ms = 1000;
int checks = 0, starts = 0, downloads = 0, writes = 0, finishes = 0, discards = 0;
constexpr char tag[] = "firmware-20261111.7";

bool safe() { return safe_now; }
void complete() { assert(task); auto run = task; task = nullptr; run(nullptr); }
void check() {
    const int before = checks;
    assert(!firmware_release_job::check(2026111106, 3342336));
    assert(firmware_release_job::busy() && !firmware_release_job::status().can_stage);
    assert(firmware_release_job::check(2026111106, 3342336));
    assert(firmware_release_job::stage(tag));
    complete();
    firmware_release_job::poll(safe);
    assert(checks == before + 1); // one check, with no second image channel
}
}

uint32_t millis() { return now_ms; }
BaseType_t xTaskCreatePinnedToCore(void (*run)(void *), const char *, unsigned, void *, unsigned, void *, int) {
    if (fail_task) return 0;
    assert(!task); task = run; return pdPASS;
}
void vTaskDelete(void *) {}

namespace firmware_ota {
Status status() { return {false, false, 3342336, 0, 0, "app0", "app1", "token", "Ready", Action::NONE}; }
const char *start(size_t bytes) { assert(bytes == 1024); ++starts; return failed_start ? "Start failed" : nullptr; }
const char *write(const uint8_t *, size_t size) {
    assert(size == 256); ++writes; return failed_write ? "Write failed" : nullptr;
}
const char *finish() { ++finishes; return failed_finish ? "Finish failed" : nullptr; }
void discard(const char *error) { assert(error && *error); ++discards; }
}

namespace firmware_release {
const char *check(uint32_t running, size_t capacity, Build &out) {
    assert(running == 2026111106 && capacity == 3342336);
    ++checks;
    out = {};
    if (failed_check) return "Secure check failed";
    if (offer) { std::strcpy(out.tag, tag); out.version = 2026111107; out.size = 1024; }
    return nullptr;
}
const char *download(const Build &build, Sink sink, void *context) {
    assert(!std::strcmp(build.tag, tag) && build.size == 1024);
    ++downloads;
    if (failed_download) return "Download failed";
    const uint8_t bytes[256] = {};
    for (int i = 0; i < 4; ++i) if (const char *error = sink(bytes, sizeof bytes, context)) return error;
    return nullptr;
}
}

int main() {
    using namespace firmware_release_job;
    check();
    assert(!std::strcmp(status().phase, "current") && !status().can_stage);
    assert(stage(tag) && !task && !starts && !downloads && !writes && !finishes);

    offer = true;
    check();
    assert(!std::strcmp(status().phase, "available") && status().can_stage);
    assert(!std::strcmp(status().build.tag, tag));
    assert(stage("firmware-20261111.8") && !starts);
    assert(!stage(tag) && starts == 1);
    assert(busy() && !status().can_stage);
    assert(!std::strcmp(ota_status().slot, "app0"));
    assert(stage(tag) && firmware_release_job::check(0, 0));
    complete(); poll(safe);
    assert(downloads == 1 && writes == 4 && finishes == 1 && !discards);
    assert(status().received == 1024 && !std::strcmp(status().phase, "staged"));
    discarded();
    assert(!std::strcmp(status().phase, "available") && status().can_stage && status().received == 0);

    failed_start = true;
    assert(stage(tag) && starts == 2 && downloads == 1 && !task);
    failed_start = false;

    failed_download = true;
    assert(!stage(tag)); complete(); poll(safe);
    assert(!std::strcmp(status().phase, "error") && !status().can_stage);
    assert(downloads == 2 && finishes == 1 && discards == 1);
    failed_download = false;

    check();
    failed_write = true;
    assert(!stage(tag)); complete(); poll(safe);
    assert(!std::strcmp(status().phase, "error") && status().received == 0);
    assert(finishes == 1 && discards == 2);
    failed_write = false;

    check();
    failed_finish = true;
    assert(!stage(tag)); complete(); poll(safe);
    assert(!std::strcmp(status().phase, "error") && finishes == 2 && discards == 3);
    failed_finish = false;

    // Readiness can disappear while HTTPS is in flight. Abort before its first
    // sink write and never finish the inactive image after cancellation.
    check();
    assert(!stage(tag));
    const int before_writes = writes, before_finishes = finishes;
    safe_now = false; now_ms += 101; poll(safe);
    complete(); poll(safe);
    assert(!std::strcmp(status().phase, "error") && !status().can_stage);
    assert(writes == before_writes && finishes == before_finishes && discards == 4);
    safe_now = true;

    // Recheck safety after the final network chunk, before making it staged.
    check(); assert(!stage(tag)); complete();
    safe_now = false; poll(safe);
    assert(!std::strcmp(status().phase, "error") && finishes == before_finishes && discards == 5);
    safe_now = true;

    failed_check = true; check();
    assert(!std::strcmp(status().phase, "error") && !status().can_stage && !status().build.tag[0]);
    assert(stage(tag) && !task);
    failed_check = false;

    fail_task = true;
    assert(firmware_release_job::check(2026111106, 3342336));
    assert(!busy() && !status().can_stage && !task);
    fail_task = false;
    puts("Single-channel release job and download safety checks passed.");
}
