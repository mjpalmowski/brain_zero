#include "restart_bluetooth.h"
#include <cassert>
#include <cstdio>

using namespace restart_bluetooth;
namespace {
int pauses, restores;
bool closed = false, pause_ok = true, restore_ok = true;
bool pause() { ++pauses; return pause_ok; }
bool quiescent() { return closed; }
bool restore() { ++restores; return restore_ok; }
const Hooks hooks{pause, quiescent, restore};
}
int main() {
    {
        Guard g;
        assert(g.poll(100, false, hooks) == Result::READY);
        g.cancel(hooks);
        assert(!pauses && !restores); // a deferred/unallocated host is untouched
    }
    {
        Guard g;
        assert(g.poll(100, true, hooks) == Result::WAIT && pauses == 1);
        assert(g.poll(101, true, hooks) == Result::WAIT && pauses == 1);
        // A disabled configuration alone is insufficient: the GAP controller
        // must acknowledge actual closure before Wi-Fi shutdown is permitted.
        assert(g.paused());
        closed = true;
        assert(g.poll(102, true, hooks) == Result::READY && pauses == 1);
        closed = false; // a late connection after cancellation must still block
        assert(g.poll(103, true, hooks) == Result::WAIT);
        g.cancel(hooks);
        assert(restores == 1 && !g.paused() && !g.restoring());
        g.cancel(hooks); assert(restores == 1); // idempotent cleanup
    }
    {
        Guard g; const int previous = restores;
        assert(g.poll(100, true, hooks) == Result::WAIT);
        assert(g.poll(100 + CLOSE_TIMEOUT_MS - 1, true, hooks) == Result::WAIT);
        assert(g.poll(100 + CLOSE_TIMEOUT_MS, true, hooks) == Result::FAILED);
        assert(restores == previous + 1 && !g.paused());
    }
    {
        Guard g; const int previous = restores;
        assert(g.poll(0, true, hooks) == Result::WAIT);
        restore_ok = false;
        g.cancel(hooks); // unsafe grid, user cancel, storage or activation failure
        assert(g.restoring() && g.paused() && restores == previous + 1);
        assert(g.poll(1, true, hooks) == Result::FAILED);
        g.retry_restore(hooks); assert(g.restoring());
        restore_ok = true; g.retry_restore(hooks);
        assert(!g.restoring() && !g.paused());
        assert(g.poll(2, true, hooks) == Result::WAIT); // next restart gets a fresh gate
        g.cancel(hooks);
    }
    {
        Guard g; pause_ok = false; const int previous = restores;
        assert(g.poll(0, true, hooks) == Result::FAILED);
        assert(restores == previous + 1 && !g.paused());
        pause_ok = true;
    }
    {
        Guard g; const uint32_t start = UINT32_MAX - 100;
        assert(g.poll(start, true, hooks) == Result::WAIT);
        assert(g.poll(start + CLOSE_TIMEOUT_MS - 1, true, hooks) == Result::WAIT);
        assert(g.poll(start + CLOSE_TIMEOUT_MS, true, hooks) == Result::FAILED);
    }
    {
        Guard g; const int previous = pauses;
        assert(g.poll(0, false, hooks) == Result::READY);
        assert(g.poll(1, true, hooks) == Result::WAIT && pauses == previous + 1);
        closed = true;
        assert(g.poll(2, true, hooks) == Result::READY);
        g.cancel(hooks); closed = false;
    }
    std::printf("Intentional restart Bluetooth closure, late GAP, timeout/wrap, all-path cancellation and restoration tests passed; Guard=%zu bytes\n", sizeof(Guard));
}
