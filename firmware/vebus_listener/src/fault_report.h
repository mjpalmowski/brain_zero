// Bounded diagnostic reads. Layout compared with Venus mk2-dbus 3.83
// on MultiPlus firmware 2623563; see docs/vebus_fault_report_2026-11-03.md.
// Only capability and FF status are used by the automatic alarm sampler.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "vebus_state.h"

namespace faults {
constexpr uint32_t TESTED_FIRMWARE = 2623563;
constexpr int64_t STATUS_PERIOD_US = 5000000, STATUS_FRESH_US = 12000000;
// Venus mk2-dbus 3.83 maps these FF bits to Alarms/HighDcVoltage and HighDcCurrent.
constexpr unsigned HIGH_DC_VOLTAGE_BIT = 10, HIGH_DC_CURRENT_BIT = 11;
constexpr int64_t TIMEOUT_US = 8000000, COOLDOWN_US = 30000000;
constexpr uint32_t RELAY_KNOWN_MASK = 0x037f3f; // includes summary bit and failing-step nibble
enum class Progress : uint8_t { NEVER, READING, DONE, PARTIAL, CANCELLED, UNSUPPORTED };
struct Report {
    Progress progress = Progress::NEVER;
    uint8_t step = 0, errors = 0;
    uint32_t attempt = 0, requests = 0, replies = 0, firmware = 0;
    int64_t started = -1, finished = -1;
    uint8_t capability = 0, valid_bits = 0, relay_count = 0;
    uint32_t extended_flags = 0, relay_code = 0, warning_flags = 0;
    bool relay_present = false;
    int64_t capability_at = -1, extended_at = -1, relay_at = -1, warnings_at = -1;
    bool active() const { return progress == Progress::READING; }
    uint8_t selector() const { const uint8_t s[] = {0x80,0xff,0xfe,0xfd}; return s[step < 4 ? step : 0]; }
    static uint32_t little(const uint8_t *p, size_t n) {
        uint32_t v = 0; for(size_t i=0;i<n && i<4;i++) v |= uint32_t(p[i]) << (i*8); return v;
    }
    bool read_capability(const uint8_t *p, size_t n, int64_t now) {
        if (!p || n != 5 || p[2] != 0xa2 || p[3] != 0x80) return false;
        capability = p[4];
        capability_at = now;
        return true;
    }
    bool read_extended(const uint8_t *p, size_t n, int64_t now) {
        if (!p || n < 5 || p[2] != 0xa2 || p[3] != 0xff || capability != 0x3f) return false;
        const unsigned bits = p[4], bytes = (bits + 7) / 8;
        if (!bits || bits > 32 || n != size_t(5 + bytes)) return false;
        valid_bits = bits;
        extended_flags = little(p + 5, bytes);
        extended_at = now;
        return true;
    }
    void begin(int64_t now, uint32_t version) {
        progress=Progress::READING;step=errors=0;started=now;finished=-1;firmware=version;attempt++;
        // Preserve earlier readings and their own ages until a fresh reply replaces them.
    }
    void cancel(int64_t now) { if(active()){progress=Progress::CANCELLED;finished=now;} }
    void reply(const uint8_t *p, size_t n, int64_t now) {
        if(!active()) return;
        bool ok=p && n>=5 && p[2]==0xa2 && p[3]==selector();
        if(ok && step==0) {
            ok=read_capability(p,n,now);
            // Treat this descriptor as opaque. Only the compared format is enabled.
            if(ok && capability!=0x3f){progress=Progress::UNSUPPORTED;finished=now;replies++;return;}
        } else if(ok && step==1) {
            ok=read_extended(p,n,now);
        } else if(ok && step==2) {
            // FE: report identifier, 24-bit LE report code, optional unrelated data.
            ok=n>=8 && n<=24;
            if(ok){relay_count=p[4];relay_code=little(p+5,3);relay_present=relay_code!=0xffffff;relay_at=now;}
        } else if(ok && step==3) {
            ok=n>=5 && n<=8;
            if(ok){warning_flags=little(p+4,n-4);warnings_at=now;}
        }
        if(ok)replies++;else errors++;
        // No retry storm and no fallback to guessed fields after a missing descriptor.
        if(step==0 && !ok){progress=Progress::PARTIAL;finished=now;return;}
        if(++step==4){progress=errors?Progress::PARTIAL:Progress::DONE;finished=now;}
    }
};
inline const char *name(Progress p) {
    switch(p){case Progress::READING:return "reading";case Progress::DONE:return "done";
    case Progress::PARTIAL:return "partial";case Progress::CANCELLED:return "cancelled";
    case Progress::UNSUPPORTED:return "unsupported";default:return "never";}
}
inline float age(int64_t t,int64_t now){return t<0 || now<t ? -1.f:float(now-t)/1e6f;}
// Only a positively matching published fault LED pattern becomes a numbered code.
// -1: stale/incomplete, -2: no qualifying pattern. Neither means /VebusError=0.
inline int led_code(const vebus::State &s,int64_t now) {
    if(!s.bus_fresh(now)||s.led_t<0||now<s.led_t||now-s.led_t>=5000000||
       s.inverter_v.t<0||now<s.inverter_v.t||now-s.inverter_v.t>=5000000||
       s.device_state_t<0||now<s.device_state_t||now-s.device_state_t>=5000000||s.led_on<0||s.led_blink<0)return -1;
    if((s.device_state!=0&&s.device_state!=2)||s.inverter_v.v>=20 || !(s.led_blink&16))return -2;
    int code=0;
    const int bit[]={4,2,8},weight[]={9,3,1};
    for(int i=0;i<3;i++){
        bool blink=s.led_blink&bit[i],on=s.led_on&bit[i];
        if(blink && on==bool(s.led_on&16))return -2; // must blink in opposite phase
        code+=weight[i]*(blink?1:on?2:0);
    }
    return code>=1&&code<=26?code:-2;
}
inline size_t to_json(const Report &r,int64_t now,int led,char *out,size_t cap) {
    int n=snprintf(out,cap,"{\"state\":\"%s\",\"active\":%s,\"attempt\":%u,\"step\":%u,\"errors\":%u,\"requests\":%u,\"replies\":%u,\"firmware\":%u,\"attempt_age_s\":%.1f,\"cooldown_s\":%.1f,\"capability\":%u,\"capability_age_s\":%.1f,\"valid_bits\":%u,\"extended_flags\":%u,\"extended_age_s\":%.1f,\"relay_present\":%s,\"relay_code\":%u,\"relay_count\":%u,\"relay_unknown_bits\":%u,\"relay_age_s\":%.1f,\"warning_flags\":%u,\"warning_unknown_bits\":%u,\"warnings_age_s\":%.1f,\"led_code\":%d}",
        name(r.progress),r.active()?"true":"false",(unsigned)r.attempt,r.step,r.errors,(unsigned)r.requests,(unsigned)r.replies,(unsigned)r.firmware,age(r.started,now),
        r.started>=0&&now>=r.started&&now-r.started<COOLDOWN_US?float(COOLDOWN_US-(now-r.started))/1e6f:0.f,
        r.capability,age(r.capability_at,now),r.valid_bits,(unsigned)r.extended_flags,age(r.extended_at,now),r.relay_present?"true":"false",
        (unsigned)r.relay_code,r.relay_count,(unsigned)(r.relay_code&~RELAY_KNOWN_MASK),age(r.relay_at,now),
        (unsigned)r.warning_flags,(unsigned)(r.warning_flags&~uint32_t(0x30001)),age(r.warnings_at,now),led);
    if(n<0 || size_t(n)>=cap){if(cap)out[0]=0;return 0;}return size_t(n);
}
}
