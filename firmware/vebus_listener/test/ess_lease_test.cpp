#include "ess_lease.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <limits.h>

namespace {
constexpr char OWNER[] = "0123456789abcdef0123456789abcdef";
constexpr char OTHER[] = "fedcba9876543210fedcba9876543210";
constexpr int64_t SECOND = 1000000, START = 5000000;

ess::Inputs inputs(bool soc = true) {
    ess::Inputs value;
    value.clock_valid = true;
    value.utc = 1794387600;
    value.local.tm_wday = 3;
    value.local.tm_hour = 23;
    value.local.tm_min = 45;
    value.soc_valid = soc;
    value.soc = 50;
    value.power_valid = true;
    value.acout_w = 500;
    value.bat_v = 53;
    value.now = START;
    value.voltage_at = START;
    return value;
}

ess::Settings settings() {
    ess::Settings value;
    value.enabled = true;
    value.mode = ess::Mode::OPTIMIZED;
    value.grid_setpoint_w = -15;
    value.safety_max_w = 5000;
    value.schedule[0].enabled = true;
    value.schedule[0].start_s = 84600;
    value.schedule[0].duration_s = 21600;
    value.schedule[0].soc = 80;
    value.grid_after_target(0, true);
    return value;
}
}

int main() {
    {
        ess_lease::Lease lease;
        assert(!lease.active(START) && !lease.view(START).id[0]);
        assert(lease.set(nullptr, 15, 2500, START));
        for (const char *bad : {"", "contains-hyphen", "ABCDEF", "ggg", "01234567890123456789012345678901234567890"})
            assert(lease.set(bad, 15, 2500, START));
        assert(lease.set(OWNER, 4, 2500, START));
        assert(lease.set(OWNER, 61, 2500, START));
        assert(lease.set(OWNER, 15, -1, START));
        assert(lease.set(OWNER, 15, 5001, START));
        assert(lease.set(OWNER, 15, 2500, -1));
        assert(lease.set(OWNER, 60, 2500, INT64_MAX - SECOND));
        assert(!lease.active(START));
        assert(!lease.set(OWNER, 15, 2500, START));
        assert(lease.view(START).remaining_s == 15);
        assert(lease.view(START + 1).remaining_s == 15);
        assert(lease.view(START + 14 * SECOND).remaining_s == 1);
        assert(lease.active(START + 15 * SECOND - 1));
        assert(!lease.active(START + 15 * SECOND));
        assert(!lease.active(START - 1));
        assert(!lease.view(START + 15 * SECOND).id[0]);
    }
    {
        ess_lease::Lease lease;
        assert(!lease.set(OWNER, 15, 2500, START));
        assert(lease.set(OTHER, 60, 5000, START + SECOND));
        assert(lease.clear(OTHER, START + SECOND));
        assert(lease.view(START + SECOND).charge_w == 2500);
        assert(lease.view(START + SECOND).remaining_s == 14);
        assert(!lease.set(OWNER, 20, 1000, START + 10 * SECOND));
        assert(lease.active(START + 29 * SECOND));
        assert(!lease.active(START + 30 * SECOND));
        assert(!lease.set(OTHER, 5, 0, START + 30 * SECOND));
        assert(!strcmp(lease.view(START + 30 * SECOND).id, OTHER));
        assert(lease.clear(OWNER, START + 31 * SECOND));
        assert(!lease.clear(OTHER, START + 31 * SECOND));
        assert(!lease.clear(OWNER, START + 32 * SECOND));
        assert(!lease.active(START + 32 * SECOND));
        assert(!lease.set(OWNER, 5, 5000, INT64_MAX - 5 * SECOND));
        assert(lease.active(INT64_MAX - 1) && !lease.active(INT64_MAX));
    }
    {
        ess_lease::Lease lease;
        auto base = settings();
        base.max_charge_w = 2500;
        base.max_discharge_w = 1500;
        assert(!lease.set(OWNER, 15, 3000, START));
        const auto effective = lease.effective(base, START);
        assert(effective.enabled && effective.mode == ess::Mode::KEEP_CHARGED);
        assert(effective.max_charge_w == 2500 && effective.max_discharge_w == 0);
        for (const auto &window : effective.schedule) assert(!window.enabled);
        assert(base.mode == ess::Mode::OPTIMIZED && base.max_charge_w == 2500 && base.max_discharge_w == 1500);
        assert(base.schedule[0].enabled && base.schedule[0].start_s == 84600 && base.schedule[0].duration_s == 21600);
        assert(base.grid_after_target(0));
        assert(lease.charge_window(base, START));
        auto in = inputs();
        in.charge_window = lease.charge_window(base, START);
        ess::Policy policy;
        const auto active = policy.decide(effective, in, 0);
        assert(active.write && active.charging && active.schedule == -1);
        assert(active.target_w == -3000 && (active.flags & ess::FLAG_NO_FEED_IN));
        const auto restored = lease.effective(base, START + 15 * SECOND);
        assert(restored.mode == base.mode && restored.max_charge_w == base.max_charge_w);
        assert(restored.max_discharge_w == base.max_discharge_w && restored.schedule[0].enabled);
        in.charge_window = lease.charge_window(base, START + 15 * SECOND);
        assert(!in.charge_window);
        const auto resumed = policy.decide(restored, in, 0);
        assert(resumed.schedule == 0); // ordinary overnight window resumes after lease expiry
    }
    {
        ess_lease::Lease lease;
        auto base = settings();
        base.safety_max_w = 2000;
        base.max_import_w = 1700;
        assert(!lease.set(OWNER, 15, 5000, START));
        const auto effective = lease.effective(base, START);
        auto in = inputs();
        in.charge_window = lease.charge_window(base, START);
        ess::Policy policy;
        auto decision = policy.decide(effective, in, 0);
        assert(decision.target_w == -1700); // existing global import limit stays authoritative
        auto uncapped_import = effective;
        uncapped_import.max_import_w = -1;
        decision = policy.decide(uncapped_import, in, 0);
        assert(decision.target_w == -2000 && decision.capped); // existing safety cap stays authoritative
        base.enabled = false;
        assert(!lease.charge_window(base, START));
        assert(!policy.decide(lease.effective(base, START), in, 0).write);
    }
    {
        ess_lease::Lease lease;
        auto base = settings();
        base.max_charge_w = -1;
        assert(!lease.set(OWNER, 15, 0, START));
        const auto effective = lease.effective(base, START);
        assert(effective.max_charge_w == 0 && effective.max_discharge_w == 0);
        assert(!lease.charge_window(base, START));
        for (bool has_soc : {false, true}) {
            auto in = inputs(has_soc);
            in.grid_hold = lease.active(START) && effective.max_charge_w == 0;
            ess::Policy policy;
            const auto decision = policy.decide(effective, in, 0);
            assert(decision.write && decision.grid_hold && decision.target_w == -500);
            assert(!decision.charging && (decision.flags & ess::FLAG_NO_FEED_IN));
        }
        base.max_charge_w = 0;
        assert(!lease.set(OWNER, 15, 2500, START));
        assert(lease.effective(base, START).max_charge_w == 0);
        assert(!lease.charge_window(base, START)); // global zero cap cannot release a voltage hold
        assert(!lease.clear(OWNER, START));
        assert(lease.effective(base, START).schedule[0].enabled);
    }
    puts("Temporary ESS lease checks passed");
}
