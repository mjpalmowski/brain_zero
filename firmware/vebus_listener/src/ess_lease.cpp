#include "ess_lease.h"

#include <limits.h>
#include <string.h>

namespace ess_lease {
namespace {
constexpr int64_t SECOND_US = 1000000;
constexpr char INVALID[] = "The request was refused.";
constexpr char OWNED[] = "Another controller is active";

bool valid_id(const char *id) {
    if (!id) return false;
    for (size_t i = 0; i <= MAX_ID; ++i) {
        const char c = id[i];
        if (!c) return i > 0;
        if (i == MAX_ID || !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return false;
}
}

bool Lease::active(int64_t now_us) const {
    return id_[0] && now_us >= started_us_ && now_us < deadline_us_;
}

const char *Lease::set(const char *id, uint32_t ttl_s, int charge_w, int64_t now_us) {
    if (!valid_id(id) || ttl_s < MIN_TTL_S || ttl_s > MAX_TTL_S ||
        charge_w < 0 || charge_w > ess::HARD_MAX_W || now_us < 0 ||
        now_us > INT64_MAX - int64_t(ttl_s) * SECOND_US) return INVALID;
    if (active(now_us) && strcmp(id_, id)) return OWNED;
    memcpy(id_, id, strlen(id) + 1);
    charge_w_ = charge_w;
    started_us_ = now_us;
    deadline_us_ = now_us + int64_t(ttl_s) * SECOND_US;
    return nullptr;
}

const char *Lease::clear(const char *id, int64_t now_us) {
    if (!valid_id(id) || now_us < 0) return INVALID;
    if (active(now_us) && strcmp(id_, id)) return OWNED;
    id_[0] = 0;
    charge_w_ = 0;
    started_us_ = deadline_us_ = 0;
    return nullptr;
}

View Lease::view(int64_t now_us) const {
    View value;
    if (!active(now_us)) return value;
    value.active = true;
    memcpy(value.id, id_, sizeof id_);
    value.charge_w = charge_w_;
    value.remaining_s = uint32_t((deadline_us_ - now_us + SECOND_US - 1) / SECOND_US);
    return value;
}

ess::Settings Lease::effective(const ess::Settings &base, int64_t now_us) const {
    ess::Settings value = base;
    if (!active(now_us)) return value;
    value.mode = ess::Mode::KEEP_CHARGED;
    value.max_charge_w = base.max_charge_w >= 0 && base.max_charge_w < charge_w_ ? base.max_charge_w : charge_w_;
    // No battery discharge during an externally requested charging window,
    // including zero-W windows used while another charger supplies the bank.
    value.max_discharge_w = 0;
    for (auto &window : value.schedule) window.enabled = false;
    return value;
}

bool Lease::charge_window(const ess::Settings &base, int64_t now_us) const {
    return active(now_us) && base.enabled && charge_w_ > 0 && base.max_charge_w != 0;
}

} // namespace ess_lease
