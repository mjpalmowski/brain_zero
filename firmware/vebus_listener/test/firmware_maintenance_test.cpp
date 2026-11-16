#include "firmware_maintenance.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <type_traits>

using namespace firmware_maintenance;
namespace {
constexpr uint32_t OFFSET = 0x340000, VERSION = 2026111604;
int checks, stages, cancels, resumes;
bool resume_ok = true, fail_launch = false;
firmware_release::Build replayed;
const char *check() { ++checks; return fail_launch ? "Check failed" : nullptr; }
const char *stage(const firmware_release::Build &b) { ++stages; replayed = b; return fail_launch ? "Stage failed" : nullptr; }
void cancel() { ++cancels; }
bool resume() { ++resumes; return resume_ok; }
const Hooks hooks{check, stage, cancel, resume};
Poll ready(uint32_t now = 1000, Job job = Job::IDLE, bool idle = true) { return {now, true, idle, nullptr, job}; }
firmware_release::Build build() {
    firmware_release::Build b;
    std::strcpy(b.tag, "firmware-20261116.5"); b.version = 2026111605; b.size = 3134567; b.preview = true;
    for (unsigned i = 0; i < sizeof b.sha256; ++i) b.sha256[i] = uint8_t(i + 1);
    return b;
}
void boot(State &s, Record &r, Action action = Action::CHECK, uint32_t now = 1000) {
    State before; const auto b = build();
    assert(!before.prepare(r, action, action == Action::STAGE ? &b : nullptr, OFFSET, VERSION));
    assert(before.pending() && !before.active());
    assert(s.boot(r, true, OFFSET, VERSION, now));
    assert(!r.magic && s.active() && !s.pending());
}
}
int main() {
    static_assert(std::is_trivial<Record>::value, "RTC storage must have no startup constructor");
    assert(sizeof(Record) == 88);
    State ordinary; Record empty{};
    ordinary.poll(empty, ready(), hooks);
    assert(!ordinary.active() && !ordinary.pending() && !checks && !stages && !resumes);
    // All corrupt/stale/non-software reset records are consumed without replay.
    for (unsigned which = 0; which < 6; ++which) {
        State before, after; Record r{};
        assert(!before.prepare(r, Action::CHECK, nullptr, OFFSET, VERSION));
        bool software = true; uint32_t offset = OFFSET, version = VERSION;
        if (which == 0) r.magic ^= 1;
        if (which == 1) r.crc ^= 1;
        if (which == 2) r.build.sha256[0] ^= 1;
        if (which == 3) software = false;
        if (which == 4) ++offset;
        if (which == 5) ++version;
        assert(!after.boot(r, software, offset, version, 0));
        assert(!after.active() && !r.magic);
        after.poll(r, ready(), hooks);
        assert(!checks && !stages && !resumes);
    }
    {
        State s; Record r{}; boot(s, r);
        State second;
        assert(!second.boot(r, true, OFFSET, VERSION, 0)); // one-shot, never a reboot loop
    }
    {
        State pending; Record r{};
        assert(pending.prepare(r, Action::STAGE, nullptr, OFFSET, VERSION));
        auto bad = build(); bad.sha256[0] = 0; std::memset(bad.sha256, 0, sizeof bad.sha256);
        assert(pending.prepare(r, Action::STAGE, &bad, OFFSET, VERSION));
        bad = build(); ++bad.version;
        assert(pending.prepare(r, Action::STAGE, &bad, OFFSET, VERSION));
        assert(!pending.prepare(r, Action::CHECK, nullptr, OFFSET, VERSION));
        assert(pending.prepare(r, Action::CHECK, nullptr, OFFSET, VERSION));
        pending.abort_request(r);
        assert(!pending.pending() && !r.magic);
    }
    {
        State s; Record r{}; boot(s, r, Action::STAGE);
        auto in = ready(); in.network_ready = false;
        s.poll(r, in, hooks); assert(!stages);
        in.network_ready = true; in.not_ready = "Stale inverter";
        s.poll(r, in, hooks); assert(!stages);
        in.not_ready = nullptr; in.worker_idle = false;
        s.poll(r, in, hooks); assert(!stages);
        in.worker_idle = true; s.poll(r, in, hooks);
        const auto b = build();
        assert(stages == 1 && !checks && !resumes);
        assert(!std::strcmp(replayed.tag, b.tag) && replayed.version == b.version && replayed.size == b.size);
        assert(replayed.preview && !std::memcmp(replayed.sha256, b.sha256, sizeof b.sha256));
        s.poll(r, ready(2000, Job::STAGED, false), hooks);
        assert(!resumes); // publication alone cannot free the TLS worker stack
        s.poll(r, ready(2000, Job::STAGED), hooks);
        assert(!std::strcmp(s.status(2000).phase, "staged"));
        s.poll(r, ready(301999, Job::STAGED), hooks); assert(!resumes);
        s.discarded(302000); s.poll(r, ready(302000, Job::AVAILABLE), hooks);
        assert(resumes == 1 && !s.active());
    }
    {
        State s; Record r{}; boot(s, r);
        s.poll(r, ready(), hooks); assert(checks == 1);
        s.poll(r, ready(2000, Job::AVAILABLE), hooks);
        assert(s.status(2000).remaining_s == 300);
        // Merely polling status never extends the maintenance deadline.
        assert(s.status(102000).remaining_s == 200);
        s.activity(102000); assert(s.status(102000).remaining_s == 300);
        s.poll(r, ready(401999, Job::AVAILABLE), hooks); assert(s.active());
        s.poll(r, ready(402000, Job::AVAILABLE), hooks); assert(!s.active() && resumes == 2);
    }
    for (Job done : {Job::CURRENT, Job::ERROR}) {
        State s; Record r{}; boot(s, r);
        s.poll(r, ready(), hooks);
        const int previous = resumes;
        s.poll(r, ready(2000, done, false), hooks); assert(resumes == previous);
        s.poll(r, ready(2000, done), hooks); assert(resumes == previous + 1 && !s.active());
    }
    {
        State s; Record r{}; boot(s, r, Action::STAGE);
        s.poll(r, ready(), hooks); s.cancel();
        const int previous = resumes;
        s.poll(r, ready(2000, Job::DOWNLOADING, false), hooks);
        s.poll(r, ready(2001, Job::DOWNLOADING, false), hooks);
        assert(cancels == 1 && resumes == previous && s.active());
        resume_ok = false;
        s.poll(r, ready(2002, Job::ERROR), hooks); assert(s.active());
        resume_ok = true;
        s.poll(r, ready(2003, Job::ERROR), hooks); assert(!s.active());
    }
    {
        State s; Record r{}; boot(s, r);
        fail_launch = true; const int previous = resumes;
        s.poll(r, ready(), hooks); assert(s.active() && resumes == previous);
        s.poll(r, ready(1001), hooks); assert(!s.active() && resumes == previous + 1);
        fail_launch = false;
    }
    {
        const uint32_t start = UINT32_MAX - 100;
        State s; Record r{}; boot(s, r, Action::CHECK, start);
        const int previous = checks;
        auto in = ready(start + IDLE_MS - 1); in.network_ready = false;
        s.poll(r, in, hooks); assert(s.active() && checks == previous);
        in.now = start + IDLE_MS;
        s.poll(r, in, hooks); assert(!s.active() && checks == previous);
    }
    std::printf("Maintenance CRC, exact-build replay, reset/version/slot/one-shot, readiness, cleanup/cancel/discard and bounded idle tests passed; State=%zu RTC=%zu bytes\n", sizeof(State), sizeof(Record));
}
