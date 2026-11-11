#pragma once

#include "ess_policy.h"

// A renewable external request changes only the effective RAM settings. The
// controller's saved ESS settings and schedules remain the fallback.
namespace ess_lease {

constexpr uint32_t MIN_TTL_S = 5, MAX_TTL_S = 60;
constexpr size_t MAX_ID = 40;

struct View {
    bool active = false;
    char id[MAX_ID + 1] = {};
    int charge_w = 0;
    uint32_t remaining_s = 0;
};

class Lease {
public:
    // Returns nullptr on success. A different owner cannot replace or clear a
    // live request; an expired lease has no owner. Times are monotonic usec.
    const char *set(const char *id, uint32_t ttl_s, int charge_w, int64_t now_us);
    const char *clear(const char *id, int64_t now_us);
    bool active(int64_t now_us) const;
    View view(int64_t now_us) const;

    ess::Settings effective(const ess::Settings &base, int64_t now_us) const;
    bool charge_window(const ess::Settings &base, int64_t now_us) const;

private:
    char id_[MAX_ID + 1] = {};
    int charge_w_ = 0;
    int64_t started_us_ = 0, deadline_us_ = 0;
};

} // namespace ess_lease
