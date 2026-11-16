#include "battery_control.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

using namespace battery_bms;
using namespace battery_control;
namespace {
constexpr int64_t NOW = 50000000;
Snapshot pack(float soc = 50, float volts = 52.8f) {
    Snapshot p; p.connected = p.complete = true;
    p.valid = SOC | VOLTAGE | CURRENT | CELL_EXTREMA | PERMISSIONS | ALARMS | CAPACITY;
    p.soc = soc; p.voltage = volts; p.current = -10; p.capacity_ah = 230;
    p.min_cell_v = 3.28f; p.max_cell_v = 3.3f; p.delta_cell_v = p.max_cell_v - p.min_cell_v;
    p.charge_allowed = p.discharge_allowed = true;
    p.sample_age_s = .3f; p.soc_age_s = .1f; p.cell_age_s = .2f; p.permissions_age_s = .2f; p.alarms_age_s = .2f;
    return p;
}
Settings enabled() {
    Settings s; s.charge_guard = true; s.max_soc = 80; s.max_cell_mv = 3450; s.max_delta_mv = 50;
    s.limited_charge_w = 100; assert(valid(s)); return s;
}
Inputs good() { const auto p = pack(); return inputs(p, p, p, NOW); }
void extrema(Inputs &in, float low, float high) {
    in.min_cell_v = low; in.max_cell_v = high; in.delta_cell_v = high - low;
}
}

int main() {
    {
        Settings s; assert(valid(s)); const auto in = good(); Guard guard;
        auto v = guard.update(s, in, NOW); assert(!v.enabled && !v.limited && v.charge_cap_w == -1);
        float selected = 123; assert(!selected_soc(s, in, NOW, selected) && selected == 123);
        assert(!selected_voltage(s, in, NOW, selected) && selected == 123);
        assert(!std::strcmp(v.reason, "Off"));
    }
    {
        auto a = pack(50, 52.8f), b = pack(63, 52.7f), bank = pack(57, 52.75f);
        auto in = inputs(a, b, bank, NOW); Settings s; float value = 0;
        s.soc_source = SocSource::JK; assert(selected_soc(s, in, NOW, value) && value == 50);
        assert(selected_voltage(s, in, NOW, value) && std::fabs(value - 52.8f) < .001f);
        s.soc_source = SocSource::PACE; assert(selected_soc(s, in, NOW, value) && value == 63);
        assert(selected_voltage(s, in, NOW, value) && std::fabs(value - 52.7f) < .001f);
        s.soc_source = SocSource::BANK; assert(selected_soc(s, in, NOW, value) && value == 57);
        assert(selected_voltage(s, in, NOW, value) && std::fabs(value - 52.75f) < .001f);
        assert(!selected_soc(s, in, NOW + FRESH_US, value)); assert(!selected_voltage(s, in, in.voltage_at[2] - 1, value));
        b.soc_age_s = 10; b.sample_age_s = .1f; in = inputs(a, b, bank, NOW);
        s.soc_source = SocSource::PACE; assert(!selected_soc(s, in, NOW, value)); assert(selected_voltage(s, in, NOW, value));
        b.connected = false; in = inputs(a, b, bank, NOW);
        assert(!selected_soc(s, in, NOW, value) && !selected_voltage(s, in, NOW, value));
        s.version = 99; assert(!selected_soc(s, good(), NOW, value));
        s.version = 1; s.soc_source = static_cast<SocSource>(255); assert(!selected_soc(s, good(), NOW, value));
        s.soc_source = SocSource::JK; in = good(); in.soc[0] = NAN; value = 123;
        assert(!selected_soc(s, in, NOW, value) && value == 123);
        in.voltage[0] = -1; assert(!selected_voltage(s, in, NOW, value) && value == 123);
        in = good(); assert(!selected_soc(s, in, in.soc_at[0] - 1, value));
    }
    {
        auto s = enabled(); auto in = good(); Guard g;
        assert(!g.update(s, in, NOW).limited);
        in.highest_soc = 80; assert(g.update(s, in, NOW).charge_cap_w == 100);
        in.highest_soc = 79.5f; assert(g.update(s, in, NOW).limited);
        in.highest_soc = 79; assert(!g.update(s, in, NOW).limited);
        extrema(in, 3.42f, 3.45f); auto v = g.update(s, in, NOW);
        assert(v.limited && !std::strcmp(v.reason, "Maximum cell voltage reached"));
        extrema(in, 3.42f, 3.445f); assert(g.update(s, in, NOW).limited);
        extrema(in, 3.42f, 3.440f); assert(!g.update(s, in, NOW).limited);
        extrema(in, 3.28f, 3.330f); v = g.update(s, in, NOW);
        assert(v.limited && !std::strcmp(v.reason, "Cell voltage difference reached"));
        extrema(in, 3.28f, 3.326f); assert(g.update(s, in, NOW).limited);
        extrema(in, 3.28f, 3.325f); assert(!g.update(s, in, NOW).limited);
    }
    {
        // Recover exact5mV boundaries despite float subtraction rounding low.
        auto s = enabled(); s.max_cell_mv = 3600; s.max_delta_mv = 5; auto in = good(); Guard g;
        extrema(in, 3.280f, 3.285f); assert(g.update(s, in, NOW).limited);
        extrema(in, 3.280f, 3.284f); assert(!g.update(s, in, NOW).limited);
        s.max_delta_mv = 6; extrema(in, 3.280f, 3.286f); assert(g.update(s, in, NOW).limited);
        extrema(in, 3.280f, 3.285f); assert(g.update(s, in, NOW).limited);
        extrema(in, 3.280f, 3.284f); assert(!g.update(s, in, NOW).limited);
    }
    {
        auto s = enabled(); auto in = good(); Guard g;
        in.highest_soc = 85; extrema(in, 3.28f, 3.46f); assert(g.update(s, in, NOW).limited);
        extrema(in, 3.28f, 3.30f); auto v = g.update(s, in, NOW);
        assert(v.limited && !std::strcmp(v.reason, "Battery SOC limit reached"));
        s.charge_guard = false; assert(g.update(s, in, NOW).charge_cap_w == -1);
        s.charge_guard = true; in.highest_soc = 79.5f; assert(!g.update(s, in, NOW).limited); // disabling clears latch
        in.charge_allowed = false; v = g.update(s, in, NOW); assert(v.limited && v.charge_cap_w == 0 && v.data_valid);
        in.charge_allowed = true; in.protections_clear = false; v = g.update(s, in, NOW);
        assert(v.limited && v.charge_cap_w == 0 && !std::strcmp(v.reason, "A battery protection is active"));
    }
    {
        auto s = enabled(); Guard g; auto a = pack(), b = pack(), bank = pack();
        b.soc = std::numeric_limits<float>::quiet_NaN();
        auto v = g.update(s, inputs(a, b, bank, NOW), NOW); assert(!v.data_valid && v.charge_cap_w == 0);
        b = pack(); b.min_cell_v = NAN; v = g.update(s, inputs(a, b, bank, NOW), NOW); assert(!v.data_valid);
        b = pack(); b.alarms_age_s = -1; v = g.update(s, inputs(a, b, bank, NOW), NOW); assert(!v.data_valid);
        b = pack(); b.valid &= ~ALARMS; v = g.update(s, inputs(a, b, bank, NOW), NOW); assert(!v.data_valid);
        b = pack(); b.permissions_age_s = 10; v = g.update(s, inputs(a, b, bank, NOW), NOW); assert(!v.data_valid);
        b = pack(); b.alarms = 1; v = g.update(s, inputs(a, b, bank, NOW), NOW); assert(v.data_valid && v.charge_cap_w == 0);
        b = pack(); b.warnings = 0xffff; v = g.update(s, inputs(a, b, bank, NOW), NOW); assert(v.data_valid && !v.limited);
        b = pack(); b.connected = false; v = g.update(s, inputs(a, b, bank, NOW), NOW); assert(!v.data_valid);
        auto in = good(); v = g.update(s, in, NOW + FRESH_US); assert(!v.data_valid);
        in = good(); in.min_cell_v = 4; in.max_cell_v = 3; in.delta_cell_v = .1f; assert(!g.update(s, in, NOW).data_valid);
        in = good(); in.delta_cell_v = .5f; assert(!g.update(s, in, NOW).data_valid);
        in = good(); in.highest_soc = NAN; assert(!g.update(s, in, NOW).data_valid);
        s.max_delta_mv = 0; assert(!g.update(s, good(), NOW).data_valid && g.update(s, good(), NOW).charge_cap_w == 0);
    }
    {
        Settings s; char why[128]; assert(valid(s, why, sizeof why) && !*why);
        s.charge_guard = true; assert(!valid(s, why, sizeof why) && *why);
        s = enabled(); s.max_soc = 101; assert(!valid(s));
        s = enabled(); s.max_cell_mv = 1999; assert(!valid(s));
        s = enabled(); s.max_cell_mv = 4501; assert(!valid(s));
        s = enabled(); s.max_delta_mv = 4; assert(!valid(s));
        s = enabled(); s.max_delta_mv = 1001; assert(!valid(s));
        s = enabled(); s.limited_charge_w = 5001; assert(!valid(s));
        s = enabled(); s.version = 2; assert(!valid(s));
        char one[1] = {'x'}; assert(!valid(s, one, 1) && one[0] == 0);
        assert(!valid(s, nullptr, 10));
    }
    std::puts("Battery selection, source voltage, disabled guard, stale/protection fail-closed and millivolt hysteresis checks passed");
}
