// Run the real job state machine with deterministic task, OTA and HTTPS seams.
#include "firmware_release_job.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include "release_stubs/freertos/task.h"

namespace {
void (*task)(void *) = nullptr;
bool site_offer = false, public_offer = false, failed_check = false;
std::vector<bool> checks;
int starts = 0, downloads = 0, writes = 0, finishes = 0;
bool downloaded_site = false;
constexpr char tag[] = "firmware-20261111.6";
constexpr char custom[] = "A public release is available. This controller needs its matching custom build to keep its extra features.";

bool safe() { return true; }
void complete() { assert(task); auto run = task; task = nullptr; run(nullptr); }
void check(bool site) {
    checks.clear();
    assert(!firmware_release_job::check(2026111105, 3342336, site));
    complete();
    firmware_release_job::poll(safe);
}
}

uint32_t millis() { return 1000; }
BaseType_t xTaskCreatePinnedToCore(void (*run)(void *), const char *, unsigned, void *, unsigned, void *, int) {
    assert(!task); task = run; return pdPASS;
}
void vTaskDelete(void *) {}

namespace firmware_ota {
Status status() { return {false, false, 3342336, 0, 0, "app0", "app1", "token", "Ready", Action::NONE}; }
const char *start(size_t) { ++starts; return nullptr; }
const char *write(const uint8_t *, size_t) { ++writes; return nullptr; }
const char *finish() { ++finishes; return nullptr; }
void discard(const char *) { assert(false); }
}

namespace firmware_release {
const char *check(uint32_t running, size_t capacity, bool site, Build &out) {
    assert(running == 2026111105 && capacity == 3342336);
    checks.push_back(site);
    out = {};
    if (failed_check) return "Secure check failed";
    if (site ? site_offer : public_offer) {
        std::strcpy(out.tag, tag); out.version = 2026111106; out.size = 1024;
    }
    return nullptr;
}
const char *download(const Build &build, bool site, Sink sink, void *context) {
    assert(!std::strcmp(build.tag, tag));
    ++downloads; downloaded_site = site;
    const uint8_t bytes[] = {1, 2, 3};
    return sink(bytes, sizeof bytes, context);
}
}

int main() {
    using namespace firmware_release_job;
    // The custom controller sees the public release, but even a direct API
    // stage call cannot start flash writes or select the public image.
    public_offer = true;
    check(true);
    assert((checks == std::vector<bool>{true, false}));
    assert(!std::strcmp(status().phase, "available"));
    assert(!std::strcmp(status().build.tag, tag) && !status().can_stage);
    assert(!std::strcmp(status().message, custom));
    assert(!std::strcmp(stage(tag), custom));
    assert(!task && !starts && !downloads && !writes && !finishes);

    // Public boards select the same metadata and retain the normal verified
    // download -> finish path. They do not check the custom asset channel.
    check(false);
    assert((checks == std::vector<bool>{false}));
    assert(status().can_stage);
    assert(!stage(tag) && starts == 1);
    complete(); poll(safe);
    assert(downloads == 1 && !downloaded_site && writes == 1 && finishes == 1);
    assert(!std::strcmp(status().phase, "staged"));

    // A matching private build retains priority and its own image channel.
    site_offer = true;
    check(true);
    assert((checks == std::vector<bool>{true}));
    assert(status().can_stage && !stage(tag));
    complete(); poll(safe);
    assert(starts == 2 && downloads == 2 && downloaded_site && finishes == 2);

    site_offer = public_offer = false;
    check(true);
    assert((checks == std::vector<bool>{true, false}));
    assert(!std::strcmp(status().phase, "current") && !status().can_stage);

    // A failed secure custom-channel check is surfaced without trying to
    // turn its failure into a selectable public image.
    failed_check = true;
    check(true);
    assert((checks == std::vector<bool>{true}));
    assert(!std::strcmp(status().phase, "error") && !status().can_stage);
    assert(starts == 2 && downloads == 2);
    puts("Release job: public discovery preserves custom controls; matching images still stage.");
}
