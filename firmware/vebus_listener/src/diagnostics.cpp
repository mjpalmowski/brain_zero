#include "diagnostics.h"
#include <stdio.h>
#include <stdarg.h>
#include <math.h>

namespace diagnostics {
// ESP-IDF esp_reset_reason_t values, kept numeric on the wire as well.
const char *reset_name(int reason) {
    switch (reason) {
    case 1: return "Power on"; case 2: return "External reset"; case 3: return "Software restart";
    case 4: return "Panic / exception"; case 5: return "Interrupt watchdog"; case 6: return "Task watchdog";
    case 7: return "Watchdog"; case 8: return "Deep sleep wake"; case 9: return "Brownout";
    case 10: return "SDIO reset"; default: return "Unknown";
    }
}
size_t to_json(const Cache &v, char *out, size_t cap) {
    if (!cap) return 0;
    size_t n=0; bool cut=false; out[0]=0;
    auto add=[&](const char *fmt,...) {
        if(cut)return;va_list args;va_start(args,fmt);int k=vsnprintf(out+n,cap-n,fmt,args);va_end(args);
        if(k<0||size_t(k)>=cap-n){cut=true;return;}n+=size_t(k);
    };
    auto number=[&](float x){if(isfinite(x))add("%.2f",double(x));else add("null");};
    add("{\"policy_age_s\":");number(v.policy_at<0||v.now<v.policy_at?-1:float(v.now-v.policy_at)/1e6f);
    add(",\"battery\":{\"pace_valid\":%s,\"pace_age_s\":",v.pace_valid?"true":"false");
    number(v.pace_age_ms==UINT32_MAX?-1:float(v.pace_age_ms)/1000.f);
    add(",\"pace_soc\":");if(v.pace_valid)number(v.pace_soc);else add("null");
    add(",\"inverter_soc\":");if(v.own_soc_valid)number(v.own_soc);else add("null");
    add(",\"sync_target\":");number(v.sync_target);
    add(",\"sync_age_s\":");number(v.sync_age_s);
    add(",\"sync_sent\":%u,\"sync_verified\":%s},",v.sync_sent,v.sync_verified?"true":"false");
    add("\"limits\":{\"max_charge_w\":%d,\"max_discharge_w\":%d,\"max_import_w\":%d,\"max_feed_in_w\":%d,\"safety_max_w\":%d,\"minimum_soc\":",
        v.max_charge_w,v.max_discharge_w,v.max_import_w,v.max_feed_in_w,v.safety_max_w);number(v.minimum_soc);add("},");
    add("\"uart\":{\"frame_errors\":%u,\"parity_errors\":%u,\"fifo_overflows\":%u,\"buffer_full\":%u,\"oversize\":%u}}",
        v.frame_errors,v.parity_errors,v.fifo_overflows,v.buffer_full,v.oversize);
    if(cut){out[0]=0;return 0;}return n;
}
}
