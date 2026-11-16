#include "restart_bluetooth.h"

namespace restart_bluetooth {
Result Guard::poll(uint32_t now, bool allocated, const Hooks &hooks) {
    if (restoring_) return Result::FAILED;
    if (!started_) {
        started_ = true; started_at_ = now;
    }
    if (allocated && !paused_) {
        // Recheck allocations even after a previously allocation-free result;
        // a late startup must not bypass the final pre-restart closure check.
        paused_ = true;
        if (!hooks.pause()) { cancel(hooks); return Result::FAILED; }
    }
    if (!paused_ || hooks.quiescent()) return Result::READY;
    if (uint32_t(now - started_at_) < CLOSE_TIMEOUT_MS) return Result::WAIT;
    cancel(hooks);
    return Result::FAILED;
}
void Guard::cancel(const Hooks &hooks) {
    started_ = false;
    if (paused_) restoring_ = true;
    retry_restore(hooks);
}
void Guard::retry_restore(const Hooks &hooks) {
    if (restoring_ && hooks.restore()) { restoring_ = false; paused_ = false; }
}
}
