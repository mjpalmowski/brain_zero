// Captured runtime IgnoreAcIn1 command and local reconnection policy. No Arduino dependencies.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <math.h>
#include <string.h>

namespace acinput {
struct Config {
    uint16_t version = 1, minutes = 30;
    float reconnect_soc = 10, reconnect_v = 50;
};
inline bool valid(const Config &c) {
    return c.version == 1 && c.minutes >= 1 && c.minutes <= 240 && isfinite(c.reconnect_soc) &&
        c.reconnect_soc >= 0 && c.reconnect_soc <= 95 && isfinite(c.reconnect_v) && c.reconnect_v >= 48 &&
        c.reconnect_v <= 56 && fabs(c.reconnect_soc-round(c.reconnect_soc)) < .0001 &&
        fabs(c.reconnect_v*10-round(c.reconnect_v*10)) < .001;
}

// One shared Requester sends these jobs. An enable is never automatically repeated.
struct Wire {
    enum Job { NONE, VERSION, WRITE, STATUS };
    bool enabled=false, wanted=false, version_known=false, supported=false, verified=false, ack=false, fault=false;
    int sent_value=-1, raw=-1, version_tries=0, release_tries=0;
    uint32_t firmware=0, writes=0, acknowledgements=0, reads=0;
    int64_t sent_at=-1, status_at=-1, next_version=0, next_write=0, next_status=0;
    const char *error="";
    int actual(int64_t now) const { return status_at >= 0 && now >= status_at && now-status_at < 8000000 ? !!(raw&0x10) : -1; }
    bool ready(int64_t now) const { return supported && sent_value==0 && verified && actual(now)==0; }
    void target(bool value) {
        enabled=true;
        if (fault && value) return;
        if (!value) fault=false;
        if (wanted != value) { wanted=value; verified=false; next_write=next_status=0; release_tries=0; }
    }
    void owner_lost() {
        wanted=false; version_known=supported=verified=ack=fault=false;
        sent_value=raw=-1; sent_at=status_at=-1; next_version=next_write=next_status=0;
        version_tries=release_tries=0; error="Another bus master is active";
    }
    Job job(int64_t now) {
        if (!enabled) return NONE;
        if (!version_known) return now>=next_version ? VERSION : NONE;
        if (!supported) return NONE;
        if (wanted && sent_value==1 && !verified && sent_at>=0 && now-sent_at>5000000) {
            wanted=false; fault=true; verified=false; release_tries=0; next_write=0;
            error="Ignore readback failed; reconnecting";
        }
        if (sent_value != int(wanted) && now>=next_write && (wanted || release_tries<3)) return WRITE;
        // Retry a release only when its actual flag did not clear. Never resend enable.
        if (!wanted && !verified && sent_value==0 && now>=next_write && release_tries<3) return WRITE;
        if (now>=next_status) return STATUS;
        return NONE;
    }
    void sent(Job j, int64_t now, int value) {
        if (j==VERSION) { version_tries++; next_version=now+5000000; }
        if (j==WRITE) {
            sent_value=value; sent_at=now; ack=verified=false; writes++;
            if (!value) release_tries++;
            next_write=now+3000000; next_status=now+100000;
        }
        if (j==STATUS) { reads++; next_status=now+((wanted||(!verified&&release_tries<3))?1000000:5000000); }
    }
    void reply(Job j, const uint8_t *p, size_t n, int64_t now) {
        if (j==VERSION) {
            if (p && n>=7 && p[2]==0x82) {
                firmware=uint32_t(p[3])|uint32_t(p[4])<<8|uint32_t(p[5])<<16|uint32_t(p[6])<<24;
                version_known=true;
                // This project/capture is the single-input MultiPlus-II 48/5000 family.
                supported=firmware/1000==2623 && firmware%1000>=502;
                error=supported?"":"Unsupported inverter firmware for Ignore AC";
            } else if (version_tries>=3) { version_known=true; error="Inverter firmware could not be verified"; }
        } else if (j==WRITE) {
            ack=p && n>=3 && p[2]==0x87;
            if (ack) acknowledgements++;
            if (p && !ack) {
                fault=true; wanted=false; error="Ignore AC write refused; reconnecting";
                if (sent_value==1) next_write=0;
            }
            next_status=now;
        } else if (j==STATUS) {
            if (p && n>=5 && p[2]==0x85) {
                raw=p[3]|p[4]<<8; status_at=now;
                verified=sent_at>=0 && now>sent_at && actual(now)==int(wanted) && sent_value==int(wanted);
                if (verified && !fault) error="";
                if (!wanted && !verified && release_tries>=3) error="Ignore flag still set after release; check other controls";
            }
        }
    }
};

struct Inputs {
    int64_t now=0;
    bool owner=false, busy=false, ess_running=false, mode_on=false, telemetry=false;
    bool soc_valid=false, voltage_valid=false, overload=false, charging=false, clock_ready=true;
    bool voltage_hold=false;
    float soc=0, voltage=0, soc_floor=0;
    int main_state=-1;
    int64_t main_at=-1;
    const char *soc_source="Unavailable";
};

class Runtime {
  public:
    Config config;
    uint32_t revision=1;
    bool requested=false;
    int64_t expires=-1, requested_at=-1, grid_since=-1, last_main=-1;
    int grid=-1;
    const char *phase="initializing", *reason="Startup: restoring grid acceptance";
    Inputs latest;
    bool can_enable=false;
    float floor=10;
    bool mode_on(int mode,int64_t observed,int64_t now) const {
        if(mode!=7)return false;
        if(observed>=0 && now>=observed && now-observed<5000000)return true;
        // Ignore transfer can emit the Multi's short break, which invalidates
        // mode readback until the next LED report. Entry already required On.
        // Tolerate only that invalidation during our first 3 s; a positive
        // changed-mode report never receives a grace period.
        return observed<0 && requested && requested_at>=0 && now>=requested_at && now-requested_at<3000000;
    }
    void cancel(const char *why) {
        if (requested) { requested=false; revision++; }
        expires=-1; reason=why;
    }
    const char *guard(const Inputs &in) const {
        if (!in.owner) return "Bus control unavailable: accepting grid";
        if (!in.ess_running) return "ESS paused: accepting grid";
        if (!in.mode_on) return "Operating mode changed or stale: accepting grid";
        if (!in.telemetry) return "Inverter data stale: accepting grid";
        if (in.voltage_hold) return "Battery voltage hold: accepting grid";
        if (!in.soc_valid) return "Battery SOC unavailable: accepting grid";
        if (!in.voltage_valid) return "Battery voltage unavailable: accepting grid";
        if (in.voltage<=config.reconnect_v) return "Low battery voltage: accepting grid";
        if (in.soc<=floor) return "Battery SOC reached reconnect floor: accepting grid";
        if (in.overload) return "High load or inverter alarm: accepting grid";
        if (in.charging) return "Charging takes priority: accepting grid";
        if (!in.clock_ready) return "Charging clock unavailable: accepting grid";
        return nullptr;
    }
    void update(const Inputs &in, const Wire &w) {
        latest=in;
        floor=fmaxf(config.reconnect_soc,in.soc_floor);
        const int actual=w.actual(in.now);
        bool main_grid=in.main_state==7 || in.main_state==8 || in.main_state==9;
        bool fresh=in.telemetry && in.main_at>=0 && in.now>=in.main_at && in.now-in.main_at<3000000;
        // Last active input/voltage alone cannot prove grid acceptance. Require
        // fresh L1 operating-state observations, clear ignore and 3 s stability.
        if (actual==0 && fresh && main_grid && w.verified && w.sent_value==0) {
            if (grid_since<0) grid_since=in.now;
            grid=in.now-grid_since>=3000000 ? 1 : -1;
        } else { grid_since=-1; grid=actual>=0&&fresh?0:-1; }
        last_main=in.main_at;
        const char *blocked=guard(in);
        if (requested) {
            if (blocked) cancel(blocked);
            else if (w.fault) cancel(w.error);
            else if (in.now>=expires) cancel("Manual duration ended: accepting grid");
        }
        can_enable=!requested && !blocked && !in.busy && w.ready(in.now) && grid==1 && in.soc>floor+2;
        if (!in.owner) phase="unavailable";
        else if (!w.version_known) phase="initializing";
        else if (!w.supported) phase="unsupported";
        else if (requested) phase=actual==1 && fresh && in.main_state>=4 && in.main_state<=6 && w.verified ? "islanded":"disconnecting";
        else if (grid==1) phase="connected";
        else if (actual==0 && fresh && !main_grid && in.voltage_valid) phase="reconnecting";
        else phase="reconnecting";
    }
    bool request(bool value, uint32_t base, const Wire &w, char *why, size_t cap) {
        const char *bad=base!=revision ? "Read current AC-input settings before applying" :
            value && !requested && !can_enable ? (guard(latest)?guard(latest):"Wait for confirmed grid connection, fresh SOC above floor + 2%, and idle controls") : nullptr;
        if (bad) {snprintf(why,cap,"%s",bad);return false;}
        if (value!=requested) {
            requested=value; revision++;
            requested_at=latest.now; expires=value?latest.now+int64_t(config.minutes)*60000000:-1;
            reason=value?"Manual Ignore AC requested":"Manual reconnect requested";
            grid_since=-1; grid=-1;
        }
        return true;
    }
};

inline size_t to_json(const Runtime &r, const Wire &w, char *out, size_t cap) {
    int a=w.actual(r.latest.now);
    char soc[32]="null",voltage[32]="null";
    if(r.latest.soc_valid)snprintf(soc,sizeof soc,"%.1f",r.latest.soc);
    if(r.latest.voltage_valid)snprintf(voltage,sizeof voltage,"%.2f",r.latest.voltage);
    int n=snprintf(out,cap,"{\"revision\":%u,\"requested\":%s,\"ignored\":%s,\"grid_connected\":%s,"
        "\"phase\":\"%s\",\"reason\":\"%s\",\"can_enable\":%s,\"soc\":%s,\"soc_source\":\"%s\",\"battery_voltage\":%s,"
        "\"effective_soc_floor\":%.1f,\"remaining_s\":%ld,\"settings\":{\"reconnect_soc\":%.0f,\"reconnect_v\":%.1f,\"minutes\":%u},"
        "\"transport\":{\"supported\":%s,\"firmware\":%u,\"raw_status\":%d,\"age_s\":%.1f,\"verified\":%s,\"acknowledged\":%s,"
        "\"writes\":%u,\"acks\":%u,\"reads\":%u,\"error\":\"%s\"}}",
        r.revision,r.requested?"true":"false",a<0?"null":a?"true":"false",r.grid<0?"null":r.grid?"true":"false",
        r.phase,r.reason,r.can_enable?"true":"false",soc,r.latest.soc_source,voltage,r.floor,
        long(r.requested && r.expires>r.latest.now?(r.expires-r.latest.now)/1000000:0),r.config.reconnect_soc,r.config.reconnect_v,r.config.minutes,
        w.supported?"true":"false",w.firmware,w.raw,w.status_at<0?-1.0:(r.latest.now-w.status_at)/1e6,
        w.verified?"true":"false",w.ack?"true":"false",w.writes,w.acknowledgements,w.reads,w.error);
    if(n<0 || size_t(n)>=cap){if(cap)out[0]=0;return 0;}return size_t(n);
}
} // namespace acinput
