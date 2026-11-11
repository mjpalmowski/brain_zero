#include "external_battery.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace external_battery {
namespace {
constexpr double CLOCK_READY = 1767225600;  // 2026-01-01 UTC
constexpr size_t MAX_PAYLOAD = 256;
enum Field : unsigned { SOC = 1, SOC_TS = 2, CURRENT = 4, CURRENT_TS = 8, VOLTAGE = 16 };
struct Sample { unsigned seen = 0; double soc = 0, soc_ts = 0, current = 0, current_ts = 0, voltage = 0; };

// The input deliberately accepts only a small flat numeric JSON schema. It
// neither allocates a JSON tree nor treats quoted, Boolean or null data as numbers.
class Parser {
public:
    Parser(const char *data, size_t size) : p_(data), end_(data + size) {}
    bool parse(Sample &out) {
        if (!take('{')) return false;
        space();
        if (p_ == end_ || *p_ == '}') return false;
        for (;;) {
            char name[24];
            if (!key(name, sizeof name) || !take(':')) return false;
            double value;
            if (!number(value)) return false;
            unsigned bit;
            if (!strcmp(name, "soc")) { bit = SOC; out.soc = value; }
            else if (!strcmp(name, "soc_ts")) { bit = SOC_TS; out.soc_ts = value; }
            else if (!strcmp(name, "external_current_a")) { bit = CURRENT; out.current = value; }
            else if (!strcmp(name, "current_ts")) { bit = CURRENT_TS; out.current_ts = value; }
            else if (!strcmp(name, "voltage")) { bit = VOLTAGE; out.voltage = value; }
            else return false;
            if (out.seen & bit) return false;
            out.seen |= bit;
            space();
            if (p_ < end_ && *p_ == '}') { ++p_; break; }
            if (!take(',')) return false;
        }
        space();
        return p_ == end_;
    }
private:
    const char *p_, *end_;
    void space() { while (p_ != end_ && (*p_ == ' ' || *p_ == '\r' || *p_ == '\n' || *p_ == '\t')) ++p_; }
    bool take(char c) { space(); if (p_ == end_ || *p_ != c) return false; ++p_; return true; }
    bool digit() const { return p_ < end_ && *p_ >= '0' && *p_ <= '9'; }
    bool key(char *out, size_t cap) {
        if (!take('"')) return false;
        size_t size = 0;
        while (p_ != end_ && *p_ != '"') {
            if (size + 1 >= cap || (*p_ != '_' && (*p_ < 'a' || *p_ > 'z'))) return false;
            out[size++] = *p_++;
        }
        out[size] = 0;
        return size && p_ < end_ && *p_++ == '"';
    }
    bool number(double &out) {
        space(); const char *start = p_;
        if (p_ < end_ && *p_ == '-') ++p_;
        if (!digit()) return false;
        if (*p_ == '0') ++p_; else while (digit()) ++p_;
        if (p_ < end_ && *p_ == '.') { ++p_; if (!digit()) return false; while (digit()) ++p_; }
        if (p_ < end_ && (*p_ == 'e' || *p_ == 'E')) {
            ++p_; if (p_ < end_ && (*p_ == '+' || *p_ == '-')) ++p_;
            if (!digit()) return false; while (digit()) ++p_;
        }
        const size_t size = size_t(p_ - start);
        if (size >= 40) return false;
        char text[40]; memcpy(text, start, size); text[size] = 0;
        char *last; out = strtod(text, &last);
        return last == text + size && isfinite(out);
    }
};

bool timestamp(double stamp, double utc, double lifetime) {
    return stamp > 0 && stamp <= utc + 5 && utc - stamp < lifetime;
}
float age(int64_t now, int64_t at, double initial) {
    return at < 0 || now < at ? -1 : float(initial + double(now - at) / 1000000.0);
}
}

void Feed::invalidate() { soc_at_ = current_at_ = -1; current_ = 0; has_voltage_ = false; }
void Feed::enabled(bool value) { if (enabled_ != value) invalidate(); enabled_ = value; }
void Feed::connected(bool value) { if (!value) invalidate(); connected_ = value; }

bool Feed::accept(const char *json, size_t length, bool retained, int64_t queued_at,
                  int64_t now_us, double utc, const char **why) {
    const auto fail = [&](const char *message) { if (why) *why = message; ++refused_; return false; };
    if (!enabled_) return fail("External battery input is disabled");
    if (!connected_ || retained || queued_at < 0 || now_us < queued_at || now_us - queued_at > 3000000)
        return fail("External battery data is retained or expired");
    if (!isfinite(utc) || utc < CLOCK_READY) return fail("Wait for the controller clock to sync");
    Sample sample;
    if (!json || !length || length > MAX_PAYLOAD || !Parser(json, length).parse(sample))
        return fail("Invalid external battery data");
    const bool soc = sample.seen & SOC, current = sample.seen & CURRENT;
    if (!soc && !current) return fail("Invalid external battery data");
    if (soc != bool(sample.seen & SOC_TS) || current != bool(sample.seen & CURRENT_TS) ||
        ((sample.seen & VOLTAGE) && !soc)) return fail("Invalid external battery data");
    if ((soc && (sample.soc < 0 || sample.soc > 100 ||
                 !timestamp(sample.soc_ts, utc, double(SOC_FRESH_US) / 1000000))) ||
        (current && (sample.current < 0 || sample.current > 200 ||
                     !timestamp(sample.current_ts, utc, double(CURRENT_FRESH_US) / 1000000))) ||
        ((sample.seen & VOLTAGE) && (sample.voltage < 3 || sample.voltage > 80)))
        return fail("External battery data is outside its range or expired");
    if ((soc && sample.soc_ts <= soc_stamp_) || (current && sample.current_ts <= current_stamp_))
        return fail("External battery data was already received");
    // Commit atomically only after every supplied value has passed validation.
    if (soc) {
        soc_ = float(sample.soc); soc_stamp_ = sample.soc_ts; soc_at_ = now_us;
        soc_initial_age_ = fmax(0, utc - sample.soc_ts);
        has_voltage_ = sample.seen & VOLTAGE; voltage_ = float(sample.voltage);
    }
    if (current) {
        current_ = float(sample.current); current_stamp_ = sample.current_ts; current_at_ = now_us;
        current_initial_age_ = fmax(0, utc - sample.current_ts);
    }
    started_ = true; ++accepted_; if (why) *why = nullptr; return true;
}

View Feed::view(int64_t now_us) const {
    View v;
    v.enabled = enabled_; v.connected = connected_;
    v.soc_age_s = age(now_us, soc_at_, soc_initial_age_);
    v.current_age_s = age(now_us, current_at_, current_initial_age_);
    v.soc_valid = enabled_ && connected_ && v.soc_age_s >= 0 && v.soc_age_s < double(SOC_FRESH_US) / 1000000;
    v.current_valid = enabled_ && connected_ && v.current_age_s >= 0 && v.current_age_s < double(CURRENT_FRESH_US) / 1000000;
    v.voltage_valid = v.soc_valid && has_voltage_;
    v.soc = soc_; v.voltage = voltage_; v.external_current_a = v.current_valid ? current_ : 0;
    v.accepted = accepted_; v.refused = refused_;
    return v;
}
}  // namespace external_battery
