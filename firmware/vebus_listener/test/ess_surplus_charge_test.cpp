// Battery voltage protection on the PC: outside the charge windows the grid may not charge the
// battery but AC-out surplus may, a hold keeps the loads on the grid while surplus still charges,
// and a latched hold releases once the battery stays above the release voltage.
#include "ess_policy.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>

using namespace ess;
namespace {
constexpr int64_t S = 1000000;

Settings protection() {
    Settings s;
    s.enabled = true;
    s.mode = Mode::OPTIMIZED;
    s.min_soc = 0;
    s.grid_setpoint_w = -15;  // export 15 W: +15 on the bus
    s.max_charge_w = 2500;
    s.safety_max_w = HARD_MAX_W;
    s.voltage_hold_cv = 4900;
    return s;
}
Inputs at(float acout_w, float bat_v, int64_t now) {
    Inputs in;
    in.clock_valid = true;
    in.local.tm_hour = 22;
    in.local.tm_min = 13;
    in.local.tm_mday = 16;
    in.local.tm_wday = 1;
    in.local.tm_yday = 319;
    in.soc_valid = true;
    in.soc = 50;
    in.power_valid = true;
    in.acout_w = acout_w;
    in.bat_v = bat_v;
    in.now = now;
    in.voltage_at = now;
    return in;
}
// Battery charge on the bus: -(setpoint + AC-out).
int charge_w(int setpoint_w, const Inputs &in) { return -(setpoint_w + int(lroundf(in.acout_w))); }
// Feed one sample a second from `from` to `to` seconds; returns the last decision.
Decision run(Policy &p, const Settings &s, float acout_w, float bat_v, int from, int to, int last = 0) {
    Decision d;
    for (int t = from; t <= to; ++t) d = p.decide(s, at(acout_w, bat_v, t * S), last);
    return d;
}
void no_grid_charging(const Decision &d, const Inputs &in) {
    assert(!(d.flags & FLAG_NO_CHARGE));
    // Import never exceeds the loads, so any charge comes from AC-out surplus.
    assert(charge_w(d.target_w, in) <= (in.acout_w < 0 ? int(lroundf(-in.acout_w)) : 0));
    assert(charge_w(d.setpoint_w, in) <= (in.acout_w < 0 ? int(lroundf(-in.acout_w)) : 0));
}
}  // namespace

int main() {
    // Outside the charge windows: surplus on AC-out charges the battery, holding the grid setpoint.
    {
        Policy p;
        const Settings s = protection();
        const Inputs in = at(-488, 51.2f, 10 * S);
        const Decision d = p.decide(s, in, 15);
        assert(d.charge_inhibited && !d.voltage_hold && !d.charging);
        no_grid_charging(d, in);
        assert(d.target_w == 15 && d.setpoint_w == 15 && charge_w(d.target_w, in) == 473);
        assert(!std::strcmp(d.why, "grid setpoint, export 15 W"));
    }
    // Without an AC-out reading surplus cannot be told from the grid: the no-charge flag stays.
    {
        Policy p;
        Inputs in = at(-488, 51.2f, 10 * S);
        in.power_valid = false;
        const Decision d = p.decide(protection(), in, 15);
        assert(d.charge_inhibited && (d.flags & FLAG_NO_CHARGE));
    }
    // An importing grid setpoint imports only what the loads draw; with surplus it imports nothing.
    {
        Settings s = protection();
        s.grid_setpoint_w = 200;
        Policy p;
        Inputs in = at(50, 51.2f, 10 * S);
        Decision d = p.decide(s, in, -50);
        no_grid_charging(d, in);
        assert(d.target_w == -50 && d.setpoint_w == -50 && charge_w(d.target_w, in) == 0);
        in = at(-300, 51.2f, 11 * S);
        d = p.decide(s, in, 0);
        no_grid_charging(d, in);
        assert(d.target_w == 0 && charge_w(d.target_w, in) == 300);
        // Without voltage protection the same setpoint still charges from the grid, as before.
        s.voltage_hold_cv = 0;
        Policy q;
        in = at(50, 51.2f, 12 * S);
        d = q.decide(s, in, -200);
        assert(!d.charge_inhibited && d.target_w == -200 && charge_w(d.target_w, in) == 150);
    }
    // Leaving a charge window, grid charging stops at once instead of ramping down.
    {
        Policy p;
        const Inputs in = at(300, 51.2f, 10 * S);
        const Decision d = p.decide(protection(), in, -2000);
        no_grid_charging(d, in);
        assert(d.target_w == 15 && d.setpoint_w == -300);
    }
    // In a charge window the grid may charge as before.
    {
        Settings s = protection();
        s.grid_setpoint_w = 200;
        Policy p;
        Inputs in = at(50, 51.2f, 10 * S);
        in.charge_window = true;
        const Decision d = p.decide(s, in, -200);
        assert(!d.charge_inhibited && !(d.flags & FLAG_NO_CHARGE));
        assert(d.target_w == -200 && charge_w(d.target_w, in) == 150);
    }
    // A latched hold: the grid supplies the loads, no discharge, surplus still charges.
    {
        Settings s = protection();
        Policy p;
        Decision d = run(p, s, 400, 48.8f, 0, 4, -400);
        assert(!d.voltage_hold);
        d = run(p, s, 400, 48.8f, 5, 5, -400);
        assert(d.voltage_hold && p.voltage_latched());
        Inputs in = at(400, 48.8f, 6 * S);
        d = p.decide(s, in, -400);
        no_grid_charging(d, in);
        assert(d.target_w == -400 && (d.flags & FLAG_NO_FEED_IN) && !d.charging);
        in = at(-488, 48.9f, 7 * S);
        d = p.decide(s, in, 0);
        no_grid_charging(d, in);
        assert(d.voltage_hold && d.target_w == 0 && charge_w(d.target_w, in) == 488 && (d.flags & FLAG_NO_FEED_IN));
        // The charge limit still applies; the rest of the surplus is exported.
        s.max_charge_w = 300;
        d = p.decide(s, in, 188);
        assert(d.target_w == 188 && charge_w(d.target_w, in) == 300);
        // Without a release voltage the hold lasts until the next charge window, however high the voltage.
        s.max_charge_w = 2500;
        d = run(p, s, 400, 53.0f, 8, 120, -400);
        assert(d.voltage_hold && p.voltage_latched());
        Inputs window = at(400, 53.0f, 121 * S);
        window.charge_window = true;
        d = p.decide(s, window, -400);
        assert(!d.voltage_hold && !p.voltage_latched() && !d.charge_inhibited);
    }
    // The release voltage ends a hold after 30 s above it; a dip or a stale sample restarts the wait.
    {
        Settings s = protection();
        s.voltage_release_cv = 5000;
        Policy p;
        Decision d = run(p, s, 400, 48.8f, 0, 5, -400);
        assert(d.voltage_hold);
        d = run(p, s, 400, 50.2f, 10, 30, -400);  // 20 s above
        assert(d.voltage_hold);
        d = run(p, s, 400, 50.0f, 31, 31, -400);  // at the release voltage, not above it
        assert(d.voltage_hold);
        d = run(p, s, 400, 50.2f, 32, 61, -400);  // 29 s above
        assert(d.voltage_hold && p.voltage_latched());
        Inputs stale = at(400, 50.2f, 64 * S);
        stale.voltage_at = 61 * S;  // 3 s old
        d = p.decide(s, stale, -400);
        assert(d.voltage_hold);
        d = run(p, s, 400, 50.2f, 65, 94, -400);  // 29 s above since the gap
        assert(d.voltage_hold);
        d = run(p, s, 400, 50.2f, 95, 95, -400);  // 30 s
        assert(!d.voltage_hold && !p.voltage_latched() && d.charge_inhibited);
        assert(!(d.flags & FLAG_NO_FEED_IN) && d.target_w == 15);  // the battery supplies the loads again
        // It latches again after 5 s at or below the threshold.
        d = run(p, s, 400, 48.9f, 100, 104, 15);
        assert(!d.voltage_hold);
        d = run(p, s, 400, 48.9f, 105, 105, 15);
        assert(d.voltage_hold);
        // A restart restores the latch; the release still applies.
        Policy restarted;
        restarted.restore_voltage_hold(s.voltage_hold_cv, true);
        d = run(restarted, s, 400, 50.5f, 200, 229, -400);
        assert(d.voltage_hold);
        d = run(restarted, s, 400, 50.5f, 230, 230, -400);
        assert(!d.voltage_hold);
    }
    // A release voltage that is not above the threshold never releases, and is refused when saved.
    {
        Settings s = protection();
        s.voltage_release_cv = 4900;
        Policy p;
        Decision d = run(p, s, 400, 48.8f, 0, 5, -400);
        d = run(p, s, 400, 53.0f, 6, 100, -400);
        assert(d.voltage_hold);
        char why[112];
        assert(!validate(s, why, sizeof why));
        s.voltage_release_cv = 8001;
        assert(!validate(s, why, sizeof why));
        s.voltage_release_cv = 0;
        assert(validate(s, why, sizeof why));
        s.voltage_release_cv = 5000;
        assert(validate(s, why, sizeof why));
        s.voltage_hold_cv = 0;  // stored for later even while the protection is off
        assert(validate(s, why, sizeof why));
    }
    // A schedule's grid hold after its target: the loads stay on the grid, surplus still charges.
    {
        Settings s = protection();
        s.voltage_hold_cv = 0;
        s.schedule[0].enabled = true;
        s.schedule[0].start_s = 23 * 3600 + 30 * 60;
        s.schedule[0].duration_s = 6 * 3600;
        s.schedule[0].soc = 40;
        s.grid_after_target(0, true);
        Policy p;
        Inputs in = at(-300, 52.0f, 10 * S);
        in.local.tm_hour = 0;
        in.local.tm_min = 30;
        in.local.tm_wday = 2;
        in.local.tm_mday = 17;
        in.local.tm_yday = 320;
        Decision d = p.decide(s, in, 0);
        assert(d.grid_hold && d.schedule == 0);
        no_grid_charging(d, in);
        assert(d.target_w == 0 && charge_w(d.target_w, in) == 300);
        in.acout_w = 400;
        d = p.decide(s, in, -400);
        no_grid_charging(d, in);
        assert(d.target_w == -400 && (d.flags & FLAG_NO_FEED_IN));
    }
    // External control outside a window with voltage protection: no grid charging either.
    {
        Settings s = protection();
        s.mode = Mode::EXTERNAL_CONTROL;
        s.grid_setpoint_w = 1000;
        Policy p;
        const Inputs in = at(200, 51.2f, 10 * S);
        const Decision d = p.decide(s, in, -200);
        assert(d.charge_inhibited);
        no_grid_charging(d, in);
        assert(d.target_w == -200);
    }
    std::puts("Voltage protection: surplus charging, no grid charging, holds and release passed");
    return 0;
}
