#include "ess_policy.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <limits>

using namespace ess;
namespace {
Settings settings(Mode mode) {
    Settings s;
    s.enabled = true;
    s.mode = mode;
    s.min_soc = 20;
    s.grid_setpoint_w = 4000;
    s.max_charge_w = 3000;
    s.safety_max_w = HARD_MAX_W;
    return s;
}
Inputs fresh() {
    Inputs in;
    in.clock_valid = true;
    in.local.tm_hour = 2;
    in.local.tm_mday = 2;
    in.local.tm_wday = 1;
    in.local.tm_yday = 1;
    in.soc_valid = true;
    in.soc = 50;
    in.power_valid = true;
    in.acout_w = 500.6f;
    in.bat_v = 52;
    return in;
}
Decision decide(const Settings &s, const Inputs &in, int last = -5000) {
    Policy p;
    return p.decide(s, in, last);
}
void bounded(const Decision &d, const Inputs &in, int cap) {
    assert(d.write);
    assert(d.target_w + double(in.acout_w) >= -cap);
    assert(d.setpoint_w + double(in.acout_w) >= -cap);
}
void prohibited(const Decision &d) {
    assert(d.flags & FLAG_NO_CHARGE);
    assert(!d.charging);
    assert(d.charge_inhibited);
}
}  // namespace

int main() {
    const Mode modes[] = {Mode::OPTIMIZED_BATTERYLIFE, Mode::OPTIMIZED,
                          Mode::KEEP_CHARGED, Mode::EXTERNAL_CONTROL};
    for (Mode mode : modes) {
        auto s = settings(mode);
        auto in = fresh();
        // The disabled default retains the existing target, ramp and flags.
        assert(in.charge_guard_w == -1);
        auto d = decide(s, in);
        assert(!(d.flags & FLAG_NO_CHARGE) && !d.charge_inhibited);
        assert(d.target_w == (mode == Mode::EXTERNAL_CONTROL ? -4000 : -3501));
        assert(d.setpoint_w == -4000);

        in.charge_guard_w = 100;
        d = decide(s, in);
        bounded(d, in, 100);
        assert(d.target_w == -600 && d.setpoint_w == -600);
        assert(!(d.flags & FLAG_NO_CHARGE));
        assert(!std::strcmp(d.why, "Battery charge limit reached"));

        // A hard stop takes effect in every mode even during a strong charge.
        in.charge_guard_w = 0;
        d = decide(s, in);
        prohibited(d);
        assert(!std::strcmp(d.why, "Battery charge limit reached"));

        // Without a fresh AC-out measurement a positive cap cannot be inferred
        // from the grid setpoint, so it prohibits charging instead.
        in.charge_guard_w = 100;
        in.power_valid = false;
        d = decide(s, in);
        prohibited(d);
        assert(!std::strcmp(d.why, "Battery data unavailable: charging paused"));
        in.power_valid = true;
        for (float invalid : {std::numeric_limits<float>::quiet_NaN(),
                              std::numeric_limits<float>::infinity(),
                              -std::numeric_limits<float>::infinity(), 1e30f, -1e30f}) {
            in.acout_w = invalid;
            prohibited(decide(s, in));
        }

        // An impossible setpoint bound must not override the safety cap.
        in = fresh();
        in.acout_w = -1000;
        in.charge_guard_w = 100;
        s.safety_max_w = 500;
        d = decide(s, in);
        prohibited(d);
        assert(d.target_w <= 500 && d.setpoint_w <= 500);

        // Invalid internal cap values fail closed rather than disabling it.
        in = fresh();
        s.safety_max_w = HARD_MAX_W;
        for (int invalid : {-2, HARD_MAX_W + 1}) {
            in.charge_guard_w = invalid;
            prohibited(decide(s, in));
        }
        // AC-out can include generation, and integer setpoints must never
        // round a fractional measurement towards more battery charging.
        for (float out : {0.4f, -0.4f, 500.4f, -500.6f, -3000.f, -6000.f, 7000.f}) {
            for (int limit : {1, 100, 1000}) {
                in.acout_w = out;
                in.charge_guard_w = limit;
                d = decide(s, in);
                if (d.flags & FLAG_NO_CHARGE) prohibited(d);
                else bounded(d, in, limit);
                assert(d.target_w >= -HARD_MAX_W && d.target_w <= HARD_MAX_W);
                assert(d.setpoint_w >= -HARD_MAX_W && d.setpoint_w <= HARD_MAX_W);
            }
        }
    }

    // Scheduled charging is bounded after the schedule has requested full power.
    for (Mode mode : {Mode::OPTIMIZED_BATTERYLIFE, Mode::OPTIMIZED}) {
        auto s = settings(mode);
        s.schedule[0].enabled = true;
        s.schedule[0].start_s = 3600;
        s.schedule[0].duration_s = 7200;
        s.schedule[0].soc = 90;
        auto in = fresh();
        in.charge_guard_w = 75;
        auto d = decide(s, in);
        assert(d.schedule == 0 && d.charging);
        bounded(d, in, 75);
        assert(d.target_w == -575 && d.setpoint_w == -575);
        in.charge_guard_w = 0;
        d = decide(s, in);
        assert(d.schedule == 0);
        prohibited(d);
    }

    // Tightening skips the normal ramp immediately; relaxing retains that ramp.
    auto s = settings(Mode::EXTERNAL_CONTROL);
    auto in = fresh();
    in.charge_guard_w = 100;
    Policy p;
    auto d = p.decide(s, in, -4500);
    bounded(d, in, 100);
    const int stopped_at = d.setpoint_w;
    in.charge_guard_w = 2000;
    d = p.decide(s, in, stopped_at);
    bounded(d, in, 2000);
    assert(d.target_w == -2500 && d.setpoint_w == stopped_at - RAMP_W);
    // A stale earlier negative setpoint is corrected even when the new normal
    // target already calls for export rather than charge.
    s.grid_setpoint_w = -2000;
    in.charge_guard_w = 100;
    d = p.decide(s, in, -4500);
    bounded(d, in, 100);
    assert(d.target_w == 2000 && d.setpoint_w == -600);

    // The guard neither bypasses the existing no-charge flag nor asks for export
    // above a normal-mode feed-in limit when AC-out generation makes it conflict.
    s = settings(Mode::EXTERNAL_CONTROL);
    s.ext_no_charge = true;
    d = decide(s, in);
    assert(d.flags & FLAG_NO_CHARGE);
    s = settings(Mode::OPTIMIZED);
    s.max_feed_in_w = 0;
    in.acout_w = -1000;
    d = decide(s, in);
    prohibited(d);
    assert(d.target_w <= 0 && d.setpoint_w <= 0);

    // A fully disabled ESS remains unable to issue any write.
    s.enabled = false;
    in.charge_guard_w = 0;
    assert(!decide(s, in).write);
    std::puts("Universal ESS charge guard: all modes, schedules, zero/stale power, safety conflicts and immediate ramp bounds passed");
}
