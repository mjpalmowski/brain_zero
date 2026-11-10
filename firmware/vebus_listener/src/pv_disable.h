// Captured RAM 132 bit 3. Shared Requester only; no independent bus writer.
#pragma once
#include "ac_input.h"

namespace pvdisable {
static constexpr int MASK=0x0008;
struct Config { uint16_t version=1, seconds=60; };
inline bool valid(const Config &c){return c.version==1 && c.seconds>=10 && c.seconds<=300;}

struct Wire {
    enum Job { NONE, READ, WRITE };
    bool enabled=false,wanted=false,supported=false,verified=false,ack=false,fault=false,enable_attempted=false;
    int raw=-1,sent_value=-1,release_tries=0;
    int64_t observed_at=-1,changed_at=0,sent_at=-1,next_read=0,next_write=0;
    uint32_t writes=0,acks=0,reads=0;
    const char *error="";
    int actual(int64_t now)const{return observed_at>=0 && now>=observed_at && now-observed_at<8000000 ? !!(raw&MASK):-1;}
    bool ready(int64_t now)const{return supported && verified && !fault && !wanted && actual(now)==0;}
    void target(bool value,int64_t now,bool refresh=false){
        if(!enabled || wanted!=value || refresh){
            enabled=true;wanted=value;changed_at=now;verified=ack=false;
            sent_value=-1;sent_at=-1;next_read=next_write=0;release_tries=0;enable_attempted=false;
            if(!value){fault=false;error="";}
        }
    }
    void fail(int64_t now,const char *why){
        target(false,now,true);fault=true;error=why;
    }
    void owner_lost(){bool e=enabled;*this=Wire();enabled=e;error="Another bus master is active";}
    void observe(int word,int64_t now){
        raw=word;observed_at=now;
        verified=now>=changed_at && actual(now)==int(wanted) && (!wanted || (sent_value==1 && now>sent_at));
        if(verified && !fault)error="";
    }
    Job job(int64_t now,bool capability,bool checked){
        supported=capability;
        if(!enabled || !supported || !checked)return NONE;
        if(actual(now)<0)verified=false;
        if(wanted && enable_attempted && !verified && now-sent_at>5000000)
            fail(now,"PV-disable readback failed; restoring PV operation");
        bool fresh=observed_at>=changed_at && actual(now)>=0 && now-observed_at<2000000;
        if(!verified && fresh && actual(now)!=int(wanted) && now>=next_write &&
           (wanted?!enable_attempted:release_tries<3))return WRITE;
        if(now>=next_read && (wanted || !verified))return READ;
        return NONE;
    }
    void reading(int64_t now){reads++;next_read=now+1000000;}
    void sent(int value,int64_t now){
        sent_value=value;sent_at=now;ack=verified=false;writes++;
        if(value)enable_attempted=true;else release_tries++;
        next_read=now+250000;next_write=now+1500000;
    }
    void reply(const uint8_t *p,size_t n,int64_t now){
        ack=p && n>=3 && p[2]==0x87;if(ack)acks++;
        if(p && !ack)fail(now,"PV-disable write refused; restoring PV operation");
        if(!wanted && release_tries>=3 && actual(now)==1)error="PV restoration not confirmed; check inverter readback";
    }
};

struct Runtime {
    Config config;
    uint32_t revision=1;
    bool requested=false,can_enable=false;
    int64_t now=0,expires=-1;
    const char *phase="initializing",*reason="Startup: restoring PV operation";
    const char *blocked="Wait for confirmed Ignore AC island operation";
    void cancel(const char *why){if(requested){requested=false;revision++;}expires=-1;reason=why;}
    void update(const acinput::Runtime &ac,const Wire &w,int64_t time){
        now=time;
        blocked=ac.guard(ac.latest);
        if(!blocked && (!ac.requested || strcmp(ac.phase,"islanded")))blocked="PV-disable requires confirmed Ignore AC island operation";
        if(!blocked && w.fault)blocked=w.error;
        if(requested){
            if(blocked)cancel(blocked);
            else if(w.actual(now)<0)cancel("PV flag readback stale: restoring PV operation");
            else if(now>=expires)cancel("Duration ended: restoring PV operation");
        }
        can_enable=!requested && !blocked && !ac.latest.busy && w.ready(now);
        int actual=w.actual(now);
        phase=!ac.latest.owner?"unavailable":!w.supported?"initializing":requested?
            (actual==1 && w.verified?"disabled":"disabling"):
            (actual==0 && w.verified?"enabled":"restoring");
    }
    bool request(bool value,uint32_t base,char *why,size_t cap){
        const char *bad=base!=revision?"PV control changed; refresh before applying":
            value && !requested && !can_enable?(blocked?blocked:"Wait for fresh PV flag readback and idle controls"):nullptr;
        if(bad){snprintf(why,cap,"%s",bad);return false;}
        // Repeated enable must never extend a running lease. Restore is always available.
        if(value!=requested){requested=value;revision++;expires=value?now+int64_t(config.seconds)*1000000:-1;}
        reason=value?"Manual PV-disable requested":"Manual PV restoration requested";
        return true;
    }
};
inline size_t to_json(const Runtime &r,const Wire &w,char *out,size_t cap){
    int a=w.actual(r.now);
    int n=snprintf(out,cap,"{\"revision\":%u,\"requested\":%s,\"disabled\":%s,\"can_enable\":%s,\"phase\":\"%s\",\"reason\":\"%s\","
        "\"blocked\":\"%s\",\"remaining_s\":%ld,\"settings\":{\"seconds\":%u},\"transport\":{\"supported\":%s,\"raw_flags\":%d,\"age_s\":%.1f,"
        "\"verified\":%s,\"acknowledged\":%s,\"writes\":%u,\"acks\":%u,\"reads\":%u,\"error\":\"%s\"}}",
        r.revision,r.requested?"true":"false",a<0?"null":a?"true":"false",r.can_enable?"true":"false",r.phase,r.reason,r.blocked?r.blocked:"",
        long(r.requested && r.expires>r.now?(r.expires-r.now)/1000000:0),r.config.seconds,w.supported?"true":"false",w.raw,
        w.observed_at<0?-1.0:(r.now-w.observed_at)/1e6,w.verified?"true":"false",w.ack?"true":"false",w.writes,w.acks,w.reads,w.error);
    if(n<0 || size_t(n)>=cap){if(cap)out[0]=0;return 0;}return size_t(n);
}
}
