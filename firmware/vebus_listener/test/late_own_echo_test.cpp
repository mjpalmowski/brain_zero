// Synthetic traffic only. Delayed processing may change ownership attribution,
// but cannot manufacture an ACK, extend a deadline or revive a failed command.
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include "requester.h"
#include "vebus_state.h"
#include "version_reply.h"

using vebus::Requester;
static int failures, cases;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %d: %s\n", __LINE__, #c); ++failures; } } while (0)
static constexpr int64_t CYCLE = 19967;

struct Frame {
    uint8_t bytes[64] = {};
    size_t size = 0;
    int64_t start = 0, end = 0;
};

struct Fixture {
    Requester r;
    uint8_t counter = 0x10;
    int64_t sync = 5000000;

    void hear_sync(int64_t at) {
        const uint8_t p[] = {0x55, 0xA9, 0x34, 0x12}; uint8_t raw[64];
        const size_t n = vebus::build_frame(0x83, 0x83, 0xFD, counter++, p, sizeof p, raw, sizeof raw);
        r.heard(raw, n, at);
    }
    Fixture() {
        r.control(true); hear_sync(1000000); hear_sync(sync-CYCLE); hear_sync(sync);
        Frame f; int64_t earliest = 0, latest = 0;
        CHECK(r.due(sync+700, f.bytes, sizeof f.bytes, &f.size, &earliest, &latest));
        CHECK(f.size >= 9 && f.bytes[6] == 0x05);
        f.start = earliest > sync+700 ? earliest : sync+700;
        f.end = f.start+int64_t(f.size)*Requester::BYTE_US+Requester::ADDRESS_PAUSE_US;
        r.sent(f.bytes, f.size, f.start, f.end); ++counter;
        CHECK(r.heard(f.bytes, f.size, f.start+100));
        uint8_t reply[16], raw[64];
        const size_t psize = version_reply(reply, f.bytes[5]);
        const size_t n = vebus::build_frame(0x83, 0x83, 0xFE, counter++, reply, psize, raw, sizeof raw);
        r.heard(raw, n, sync+16080);
        CHECK(r.inverter_firmware() == 2623563 && !r.firmware_gate(sync+17000));
    }
    Frame pending(bool actual_send = true) {
        Frame f;
        for (int i = 0; i < 200; ++i) {
            sync += CYCLE; hear_sync(sync);
            int64_t early = 0, late = 0;
            if (r.due(sync+700, f.bytes, sizeof f.bytes, &f.size, &early, &late)) {
                f.start = early > sync+700 ? early : sync+700;
                f.end = f.start+int64_t(f.size)*Requester::BYTE_US+Requester::ADDRESS_PAUSE_US;
                if (actual_send) {
                    r.sent(f.bytes, f.size, f.start, f.end); ++counter;
                    uint8_t p[32]; const size_t n = vebus::payload_of(f.bytes, f.size, p, sizeof p);
                    if (n >= 6 && p[0] == 0x21 && p[5] == 0x0E) {
                        // Consume the initial no-reply snapshot trigger so timeout
                        // scenarios exercise a real read with a reply deadline.
                        CHECK(r.heard(f.bytes, f.size, f.start+100));
                        r.tick(sync+19000); continue;
                    }
                }
                return f;
            }
            r.tick(sync+19000);
        }
        CHECK(false); return f;
    }
    bool echo(const Frame &f, int64_t at) { return r.heard(f.bytes, f.size, at); }
    void require_foreign(const Frame &f, int64_t at, bool paused = true) {
        CHECK(!echo(f, at));
        CHECK(!r.inverter_firmware());
        if (paused) CHECK(r.control_state(at) == Requester::CtlState::PAUSED);
    }
};

static Frame altered(const Frame &f, unsigned change) {
    Frame other = f; uint8_t p[32];
    size_t n = vebus::payload_of(f.bytes, f.size, p, sizeof p);
    uint8_t a = f.bytes[0], b = f.bytes[1], number = f.bytes[3];
    if (change == 0) a = 0x8F;
    if (change == 1) p[n-1] ^= 1;
    if (change == 2) number = uint8_t((number+1)&0x7F);
    other.size = vebus::build_frame(a, b, f.bytes[2], number, p, n, other.bytes, sizeof other.bytes);
    if (change == 3) other.bytes[other.size-2] ^= 1; // damaged checksum
    if (change == 4) --other.size; // incomplete frame
    return other;
}

static void attribution_and_deadlines() {
    for (int64_t delay : {int64_t(250), int64_t(2000), int64_t(60000)}) {
        for (bool timeout_first : {false, true}) {
            if (timeout_first && delay < 60000) continue;
            ++cases; Fixture f; const Frame tx = f.pending();
            const auto before = f.r.ctl_stats();
            if (timeout_first) f.r.tick(tx.end+Requester::ECHO_TIMEOUT_US+1);
            CHECK(f.echo(tx, tx.start+delay));
            vebus::Decoder decoder; decoder.feed(tx.bytes, tx.size, tx.start+delay, true);
            CHECK(decoder.state().master_frames == 0 && decoder.state().own_frames == 1);
            CHECK(f.r.inverter_firmware() == 2623563 && !f.r.firmware_gate(tx.start+delay));
            CHECK(f.r.ctl_stats().answered == before.answered && f.r.ctl_stats().acked == before.acked);
            CHECK(f.r.ctl_stats().late_own_echoes == before.late_own_echoes+unsigned(delay>Requester::ECHO_START_US));
            if (delay > Requester::ECHO_START_US) {
                // Attribution does not satisfy the original echo or reply deadlines.
                f.r.tick(tx.end+Requester::REPLY_TIMEOUT_US+1);
                CHECK(f.r.ctl_stats().failed == before.failed+1);
                CHECK(f.r.ctl_stats().unanswered == before.unanswered+1);
                CHECK(f.r.ctl_stats().answered == before.answered && f.r.ctl_stats().acked == before.acked);
                CHECK(f.r.inverter_firmware() == 2623563);
            }
        }
    }
    for (int64_t edge : {int64_t(-1), int64_t(0), int64_t(1)}) {
        ++cases; Fixture f; const Frame tx = f.pending();
        const int64_t at = tx.end+Requester::REPLY_TIMEOUT_US+edge;
        if (edge <= 0) CHECK(f.echo(tx, at));
        else f.require_foreign(tx, at);
    }
    ++cases; Fixture f; const Frame tx = f.pending();
    f.r.tick(tx.end+Requester::REPLY_TIMEOUT_US+1);
    f.require_foreign(tx, tx.end+Requester::REPLY_TIMEOUT_US+2);
}

static void mismatches_and_replays() {
    for (unsigned change = 0; change < 5; ++change) {
        ++cases; Fixture f; const Frame tx = f.pending();
        f.require_foreign(altered(tx, change), tx.start+2000);
        CHECK(f.r.ctl_stats().late_own_echoes == 0);
        CHECK(!f.echo(tx, tx.start+3000)); // bad traffic consumed the pending attribution
    }
    for (int64_t first : {int64_t(100), int64_t(2000)}) {
        ++cases; Fixture f; const Frame tx = f.pending();
        CHECK(f.echo(tx, tx.start+first));
        f.require_foreign(tx, tx.start+first+2000); // timely and delayed attribution are both single-use
    }
    for (unsigned advance : {1u, 2u}) {
        ++cases; Fixture f; const Frame tx = f.pending();
        const uint8_t p[] = {0xE4, 0x12, 0x34, 0x56}; uint8_t raw[64];
        const size_t n = vebus::build_frame(0x83, 0x83, 0xFE, uint8_t((tx.bytes[3]+advance)&0x7F), p, sizeof p, raw, sizeof raw);
        bool counted = false; CHECK(!f.r.heard(raw, n, tx.start+15000, &counted));
        CHECK(counted == (advance == 1));
        CHECK(f.r.inverter_firmware() == 2623563);
        f.require_foreign(tx, tx.start+16000); // first nonmatching traffic retires the candidate
    }
}

static void only_actual_latest_send() {
    for (bool gave_up : {false, true}) {
        ++cases; Fixture f; const Frame tx = f.pending(false);
        if (gave_up) f.r.gave_up(true);
        f.require_foreign(tx, tx.start+2000); // due() alone never proves transmission
    }
    for (bool old_frame : {false, true}) {
        ++cases; Fixture f; const Frame old = f.pending(); Frame newest = altered(old, 2);
        newest.start = old.end+1000;
        newest.end = newest.start+int64_t(newest.size)*Requester::BYTE_US+Requester::ADDRESS_PAUSE_US;
        f.r.sent(newest.bytes, newest.size, newest.start, newest.end);
        if (old_frame) f.require_foreign(old, newest.start+2000);
        else CHECK(f.echo(newest, newest.start+2000));
    }
    for (unsigned invalidation = 0; invalidation < 4; ++invalidation) {
        ++cases; Fixture f; const Frame tx = f.pending();
        if (invalidation == 0) { f.r.control(false); f.r.control(true); }
        if (invalidation == 1) f.r.start(tx.start+500, 1, Requester::Mode::READ);
        if (invalidation == 2) {
            const uint8_t brk[] = {0, 0xFB, 0x30, 0x4F, 0xFF};
            f.r.heard(brk, sizeof brk, tx.start+1000);
        }
        if (invalidation == 3) {
            f.r.start(tx.start+500, 1, Requester::Mode::READ);
            const Frame test_tx = f.pending(); f.r.stop("finished synthetic session");
            f.require_foreign(test_tx, test_tx.start+2000); continue;
        }
        f.require_foreign(tx, tx.start+2000, invalidation != 1);
    }
    ++cases; Fixture still_pending; const Frame sent = still_pending.pending();
    uint8_t unused[64]; size_t n = 0; int64_t early = 0, late = 0;
    CHECK(!still_pending.r.due(sent.end+100, unused, sizeof unused, &n, &early, &late));
    CHECK(still_pending.echo(sent, sent.start+2000)); // a denied due() does not replace an actual send

    ++cases; Fixture invalid; Frame bad = altered(invalid.pending(), 3);
    invalid.r.sent(bad.bytes, bad.size, bad.start, bad.end);
    invalid.require_foreign(bad, bad.start+2000); // even sent() cannot arm invalid-checksum bytes
}

// Count an opaque synthetic access token only inside the requester object.
// The existing last-frame buffer retains one copy for decoder bookkeeping;
// the additional attribution copy must disappear immediately when retired.
static unsigned copies(const Requester &r, const uint8_t *needle, size_t size) {
    const auto *bytes = reinterpret_cast<const unsigned char *>(&r); unsigned count = 0;
    for (size_t at = 0; at+size <= sizeof r; ++at) if (!std::memcmp(bytes+at, needle, size)) ++count;
    return count;
}

static void attribution_copy_erased() {
    for (unsigned retire = 0; retire < 6; ++retire) {
        ++cases; Fixture f; Frame tx = f.pending();
        const uint8_t access[] = {0, 0x91, 0x3A, 0xD1, 0x62, 0xB3, 0x74, 0xC5, 0x26, 0x97, 0x48};
        tx.size = vebus::build_frame(0x98, 0xF7, 0xFE, 0x62, access, sizeof access, tx.bytes, sizeof tx.bytes);
        tx.end = tx.start+int64_t(tx.size)*Requester::BYTE_US+Requester::ADDRESS_PAUSE_US;
        f.r.sent(tx.bytes, tx.size, tx.start, tx.end);
        CHECK(copies(f.r, access+3, 8) == 2);
        if (retire == 0) CHECK(f.echo(tx, tx.start+100));
        if (retire == 1) CHECK(f.echo(tx, tx.start+2000));
        if (retire == 2) f.r.tick(tx.end+Requester::REPLY_TIMEOUT_US+1);
        if (retire == 3) f.r.control(false);
        if (retire == 4) {
            const uint8_t p[] = {0xE4, 0, 0, 0}; uint8_t raw[64];
            const size_t n = vebus::build_frame(0x83, 0x83, 0xFE, 0x63, p, sizeof p, raw, sizeof raw);
            f.r.heard(raw, n, tx.start+15000);
        }
        if (retire == 5) {
            const uint8_t p[] = {0, 0x92, 0x30, 13, 14}; Frame newer = tx;
            newer.size = vebus::build_frame(0x98, 0xF7, 0xFE, 0x63, p, sizeof p, newer.bytes, sizeof newer.bytes);
            f.r.sent(newer.bytes, newer.size, tx.end+1000, tx.end+2000);
        }
        CHECK(copies(f.r, access+3, 8) == (retire == 5 ? 0u : 1u));
    }
}

static void failed_command_not_revived() {
    ++cases; Fixture f;
    vebus::State state; const int64_t now = f.sync+17000;
    state.last_multi = state.led_t = state.actual_mode_t = state.limit_a.t = now;
    state.actual_mode = 7; state.ac_input_config = 4;
    state.limit_a.v = 10; state.limit_min_a.v = 0; state.limit_max_a.v = 50;
    inverter::Command command; command.action = inverter::Action::CURRENT; command.value = 200;
    char why[180]; CHECK(f.r.operate(now, command, state, why, sizeof why));
    const Frame tx = f.pending();
    CHECK(f.r.inverter_view(tx.start, state).job.progress == inverter::Progress::SENDING);
    f.r.tick(tx.end+Requester::ECHO_TIMEOUT_US+1);
    CHECK(f.r.inverter_view(tx.end+Requester::ECHO_TIMEOUT_US+1, state).job.progress == inverter::Progress::FAILED);
    const auto before = f.r.ctl_stats();
    CHECK(f.echo(tx, tx.start+60000));
    CHECK(f.r.inverter_view(tx.start+60000, state).job.progress == inverter::Progress::FAILED);
    CHECK(f.r.ctl_stats().failed == before.failed && f.r.ctl_stats().acked == before.acked);
}

static void decoder_counts_late_echo_once() {
    ++cases; Fixture f; const Frame tx = f.pending(); vebus::Decoder decoder;
    CHECK(f.echo(tx, tx.start+2000)); decoder.feed(tx.bytes, tx.size, tx.start+2000, true);
    const uint8_t e4[] = {0xE4, 0x12, 0x34, 0x56}; uint8_t raw[64];
    size_t n = vebus::build_frame(0x83, 0x83, 0xFE, uint8_t((tx.bytes[3]+1)&0x7F), e4, sizeof e4, raw, sizeof raw);
    bool counted = false; const bool own = f.r.heard(raw, n, tx.start+15030, &counted);
    // Reproduce main.cpp's exact decoder handoff, including missing-echo backfill.
    if (counted) {
        size_t size = 0; int64_t start = 0; const uint8_t *sent = f.r.sent_frame(&size, &start);
        decoder.feed(sent, size, start, true);
    }
    decoder.feed(raw, n, tx.start+15030, own);
    CHECK(!counted && decoder.state().frames == 2 && decoder.state().own_frames == 1 &&
          decoder.state().master_frames == 0 && decoder.state().gaps == 0);

    // The same E4 must still perform normal logical counting; a genuine later
    // inverter reply completes the read, rather than the late echo doing so.
    uint8_t request[32], reply[16] = {}; const size_t q = vebus::payload_of(tx.bytes, tx.size, request, sizeof request);
    size_t size = 0;
    if (request[0] == 0) {
        reply[0] = 0; reply[1] = request[1]; reply[2] = request[2] == 0x38 ? 0x99 : 0x85; size = 9;
    } else if (q >= 7 && request[0] == 0x21 && request[5] == 8) {
        reply[0] = 0x20; reply[5] = request[6] == 0xFF ? 0x0C : 0x08; size = 15;
    } else CHECK(false);
    const auto before = f.r.ctl_stats();
    n = vebus::build_frame(0x83, 0x83, 0xFE, uint8_t((tx.bytes[3]+2)&0x7F), reply, size, raw, sizeof raw);
    CHECK(!f.r.heard(raw, n, tx.start+16080));
    CHECK(f.r.ctl_stats().answered == before.answered+1 && f.r.inverter_firmware() == 2623563);
}

static void decoder_backfill_and_foreign_still_work() {
    ++cases; Fixture missing; const Frame tx = missing.pending(); vebus::Decoder decoder;
    const uint8_t e4[] = {0xE4, 0x12, 0x34, 0x56}; uint8_t raw[64];
    size_t n = vebus::build_frame(0x83, 0x83, 0xFE, uint8_t((tx.bytes[3]+1)&0x7F), e4, sizeof e4, raw, sizeof raw);
    bool counted = false; const bool own = missing.r.heard(raw, n, tx.start+15030, &counted);
    CHECK(counted && !own); // no received echo still requires the existing decoder backfill
    if (counted) {
        size_t size = 0; int64_t start = 0; const uint8_t *sent = missing.r.sent_frame(&size, &start);
        decoder.feed(sent, size, start, true);
    }
    decoder.feed(raw, n, tx.start+15030, own);
    CHECK(decoder.state().frames == 2 && decoder.state().own_frames == 1 &&
          decoder.state().master_frames == 0 && decoder.state().gaps == 0);

    ++cases; Fixture foreign; const Frame before = foreign.pending();
    CHECK(foreign.echo(before, before.start+2000));
    // Matching the next sequence number is not permission for another master.
    // Even immediately after a late own echo, foreign traffic must invalidate.
    const uint8_t payload[] = {0, 0x90, 0x35, 1};
    n = vebus::build_frame(0x8F, 0xF7, 0xFE, uint8_t((before.bytes[3]+1)&0x7F), payload, sizeof payload, raw, sizeof raw);
    counted = true;
    const bool foreign_own = foreign.r.heard(raw, n, before.start+15030, &counted);
    CHECK(!foreign_own && !counted && !foreign.r.inverter_firmware());
    CHECK(foreign.r.control_state(before.start+15030) == Requester::CtlState::PAUSED);
    vebus::Decoder observed; observed.feed(raw, n, before.start+15030, foreign_own);
    CHECK(observed.state().master_frames == 1 && observed.state().own_frames == 0);
}

int main() {
    attribution_and_deadlines(); mismatches_and_replays(); only_actual_latest_send();
    attribution_copy_erased(); failed_command_not_revived(); decoder_counts_late_echo_once();
    decoder_backfill_and_foreign_still_work();
    std::printf("Late exact own-echo ownership: %d scenarios, %d failures.\n", cases, failures);
    return failures ? 1 : 0;
}
