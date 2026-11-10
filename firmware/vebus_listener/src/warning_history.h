#pragma once

#include <stdint.h>
#include <string.h>
#include "fault_report.h"

namespace warnings {
// Append new kinds so the saved history keeps its existing identifiers.
enum Kind : uint8_t {
    LOW_BATTERY, OVERLOAD, TEMPERATURE, DC_RIPPLE, BUS_ERROR, MAINS_LOST,
    VOLTAGE_SENSE, TEMPERATURE_SENSE, HIGH_DC_VOLTAGE, HIGH_DC_CURRENT, AC_NOT_ACCEPTED, KIND_COUNT
};
enum Level : int8_t { UNKNOWN = -1, CLEAR, PREALARM, ALARM };
constexpr uint32_t CAPACITY = 32;
constexpr uint32_t STORAGE_VERSION = 1;
constexpr float MAINS_ABSENT_V = 20.0f;
constexpr int64_t INPUT_ALARM_DELAY_US = 5000000;

struct Live {
    bool leds_known = false;
    int led_code = -1;
    int mains_lost = -1; // unknown until a fresh AC-in voltage is available
    Level levels[KIND_COUNT];

    Live() { for (auto &level : levels) level = UNKNOWN; }
};

inline bool fresh(int64_t at, int64_t now, int64_t window = 5000000) {
    return at >= 0 && now >= at && now - at < window;
}

inline Live decode(const vebus::State &s, int64_t now, const faults::Report &report = {}) {
    Live live;
    const bool bus_known = s.bus_fresh(now);
    // Use the voltage at AC-in, independently of whether the transfer relay accepts it.
    if (bus_known && fresh(s.mains_v.t, now) && s.mains_v.v >= 0) {
        live.mains_lost = s.mains_v.v < MAINS_ABSENT_V;
        live.levels[MAINS_LOST] = live.mains_lost ? PREALARM : CLEAR;
    }
    if (bus_known && report.firmware == faults::TESTED_FIRMWARE && report.capability == 0x3f &&
        fresh(report.extended_at, now, faults::STATUS_FRESH_US)) {
        const auto flag = [&report](unsigned bit) {
            if (report.valid_bits <= bit) return UNKNOWN;
            return report.extended_flags & (1u << bit) ? ALARM : CLEAR;
        };
        live.levels[HIGH_DC_VOLTAGE] = flag(faults::HIGH_DC_VOLTAGE_BIT);
        live.levels[HIGH_DC_CURRENT] = flag(faults::HIGH_DC_CURRENT_BIT);
    }
    live.leds_known = bus_known && fresh(s.led_t, now) &&
                      s.led_on >= 0 && s.led_on <= 255 && s.led_blink >= 0 && s.led_blink <= 255;
    if (!live.leds_known) return live;
    live.led_code = faults::led_code(s, now);
    if (live.led_code != -1) live.levels[BUS_ERROR] = live.led_code > 0 ? ALARM : CLEAR;
    constexpr int overload = 1 << 5, low = 1 << 6, temp = 1 << 7, ripple = overload | low;
    const int on = s.led_on, blink = s.led_blink;
    live.levels[LOW_BATTERY] = live.levels[OVERLOAD] = live.levels[DC_RIPPLE] = CLEAR;
    // Opposite-phase flashes represent separate overload and low-battery warnings.
    if ((blink & ripple) == ripple && bool(on & overload) == bool(on & low)) {
        live.levels[DC_RIPPLE] = PREALARM;
    } else if ((on & ripple) == ripple && !(blink & ripple)) {
        live.levels[DC_RIPPLE] = ALARM;
    } else {
        live.levels[LOW_BATTERY] = blink & low ? PREALARM : on & low ? ALARM : CLEAR;
        live.levels[OVERLOAD] = blink & overload ? PREALARM : on & overload ? ALARM : CLEAR;
    }
    live.levels[TEMPERATURE] = blink & temp ? PREALARM : on & temp ? ALARM : CLEAR;
    constexpr int absorption = 2, bulk = 4, floating = 8, inverting = 16;
    const int charger_blink = blink & (absorption | bulk | floating);
    // Both charger LEDs must flash together. Exclude equalization and the
    // ternary VE.Bus code, whose charger LEDs flash opposite to the inverter.
    const auto sense = [&](int pair) {
        if (charger_blink != pair || ((on & pair) != 0 && (on & pair) != pair) ||
            ((blink & inverting) && bool(on & inverting) != bool(on & absorption)) ||
            live.led_code > 0) return CLEAR;
        if (!fresh(s.charge_state_t, now)) return UNKNOWN;
        return s.charge_state == 7 ? CLEAR : ALARM;
    };
    live.levels[VOLTAGE_SENSE] = sense(bulk | absorption);
    live.levels[TEMPERATURE_SENSE] = sense(absorption | floating);
    return live;
}

struct InputWatch {
    int64_t rejected_since = -1;

    Level sample(const vebus::State &s, const Live &live, int64_t now, bool requested, int ignored) {
        const auto reset = [this](Level level) { rejected_since = -1; return level; };
        if (requested || ignored == 1 || live.mains_lost == 1) return reset(CLEAR);
        if (!s.bus_fresh(now) || !fresh(s.actual_mode_t, now)) return reset(UNKNOWN);
        if (s.actual_mode == 4 || s.actual_mode == 6) return reset(CLEAR);
        if (ignored != 0 || live.mains_lost != 0 || !fresh(s.device_state_t, now, 3000000) ||
            (s.actual_mode != 5 && s.actual_mode != 7)) return reset(UNKNOWN);
        if (s.device_state >= 7 && s.device_state <= 9) return reset(CLEAR);
        if (rejected_since < 0) rejected_since = now;
        return now - rejected_since >= INPUT_ALARM_DELAY_US ? ALARM : UNKNOWN;
    }
};

struct Stamp { uint32_t utc, uptime_s, boot; };
struct Entry {
    Stamp appeared, cleared;
    uint8_t kind, level, code, closed;
};
struct Stored {
    uint32_t version, count;
    Entry entries[CAPACITY];
};

struct History {
    Stored data{STORAGE_VERSION, 0, {}};
    uint32_t revision = 0;
    bool dirty = false;

    void restore(const Stored &saved) {
        if (saved.version != STORAGE_VERSION || saved.count > CAPACITY) return;
        unsigned open = 0;
        for (uint32_t i = 0; i < saved.count; ++i) {
            const Entry &e = saved.entries[i];
            if (e.kind >= KIND_COUNT || e.level < PREALARM || e.level > ALARM || e.closed > 1) return;
            if (e.kind == BUS_ERROR ? (e.code < 1 || e.code > 26 || e.level != ALARM) : e.code != 0) return;
            if (!e.closed) {
                const unsigned bit = 1u << e.kind;
                if (open & bit) return;
                open |= bit;
            }
        }
        data = saved;
    }

    void observe(const Live &live, Stamp now) {
        bool changed = false;
        for (uint8_t kind = 0; kind < KIND_COUNT; ++kind) {
            const Level level = live.levels[kind];
            if (level == UNKNOWN) continue;
            const uint8_t code = kind == BUS_ERROR && level == ALARM ? uint8_t(live.led_code) : 0;
            Entry *active = nullptr;
            for (uint32_t i = 0; i < data.count; ++i) {
                Entry &e = data.entries[i];
                if (e.kind == kind && !e.closed) { active = &e; break; }
            }
            if (active && active->level == level && active->code == code) continue;
            if (active) {
                active->cleared = now;
                active->closed = 1;
                changed = true;
            }
            if (level != CLEAR) {
                append({now, {}, kind, uint8_t(level), code, 0});
                changed = true;
            }
        }
        // Once NTP is available, date any earlier observations from this boot.
        if (now.utc) {
            for (uint32_t i = 0; i < data.count; ++i) {
                Entry &e = data.entries[i];
                changed |= date_stamp(e.appeared, now);
                if (e.closed) changed |= date_stamp(e.cleared, now);
            }
        }
        if (changed) { ++revision; dirty = true; }
    }

private:
    static bool date_stamp(Stamp &stamp, Stamp now) {
        if (stamp.utc || stamp.boot != now.boot || stamp.uptime_s > now.uptime_s) return false;
        const uint32_t elapsed = now.uptime_s - stamp.uptime_s;
        if (elapsed >= now.utc) return false;
        stamp.utc = now.utc - elapsed;
        return true;
    }

    void append(const Entry &entry) {
        if (data.count == CAPACITY) {
            // At most KIND_COUNT entries are open. Retain them until they clear.
            uint32_t oldest = 0;
            while (oldest < data.count && !data.entries[oldest].closed) ++oldest;
            if (oldest == data.count) return;
            memmove(data.entries + oldest, data.entries + oldest + 1,
                    (data.count - oldest - 1) * sizeof(Entry));
            --data.count;
        }
        data.entries[data.count++] = entry;
    }
};
}  // namespace warnings
