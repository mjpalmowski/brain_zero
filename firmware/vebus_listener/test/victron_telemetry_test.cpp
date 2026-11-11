#include "victron_telemetry.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <initializer_list>

using namespace gx;
static vebus::Reading reading(float v) { vebus::Reading r; r.v=v; r.t=8000000; return r; }
int main(int argc, char **argv) {
    Identity id;const uint8_t mac[]={0x02,0,0,1,2,3};identity(mac,"test firmware",id);
    assert(!strcmp(id.installation,"020000010203"));
    Snapshot s;s.now=9000000;s.bus.last_multi=8900000;s.utc=1793524000;s.external_soc_valid=true;s.external_voltage_valid=true;s.external_soc=21;s.external_voltage=54;
    s.bus.soc=reading(19.5);s.bus.bat_v=reading(52);s.bus.bat_a=reading(-10);
    s.bus.mains_v=s.bus.inverter_v=reading(240);s.bus.mains_a=reading(1.5);s.bus.inverter_a=reading(3.5);
    s.bus.acin_w=reading(-123);s.bus.acout_w=reading(-200);s.bus.ac_hz=reading(49.948f);
    char topic[160],payload[384];Request r;
    assert(read_request(id,"R/020000010203/keepalive",r)&&r.keepalive);
    for(const char *bad:{"W/020000010203/vebus/288/Mode","R/d83add0396a4/keepalive","R/020000010203/vebus/28","R/020000010203/#","R/020000010203/+/0","R/020000010203/not-a-path"})
        assert(!read_request(id,bad,r));
    assert(read_request(id,"R/020000010203/vebus/288/Ac/ActiveIn",r));int selected=0;
    for(int i=0;i<topic_count();i++)if(matches(r,i))++selected;
    assert(selected==4&&topic_count()==28&&sensor_count()==17);
    FILE *f=argc>1?fopen(argv[1],"w"):nullptr;if(argc>1)assert(f);
    int found=0;
    for(int i=0;i<topic_count();i++) {
        assert(notification(i,id,s,topic,sizeof topic,payload,sizeof payload));
        assert(!strstr(topic,"/Mode")&&!strstr(topic,"/Settings/")&&!strstr(topic,"/CurrentLimit"));
        if(strstr(topic,"/Ac/ActiveIn/L1/P")){assert(!strcmp(payload,"{\"value\":123.000}"));++found;}
        if(strstr(topic,"/Dc/0/Power")){assert(!strcmp(payload,"{\"value\":-520.000}"));++found;}
        if(strstr(topic,"/Ac/Out/L1/P")){assert(!strcmp(payload,"{\"value\":-200.000}"));++found;}
        if(strstr(topic,"/system/0/Dc/Battery/Soc")){assert(!strcmp(payload,"{\"value\":21.000}"));++found;}
        if(f)fprintf(f,"%s\t%s\n",topic,payload);
    }
    assert(found==4);if(f)fclose(f);
    assert(!notification(0,id,s,topic,8,payload,sizeof payload));
    assert(!notification(0,id,s,topic,sizeof topic,payload,8));
    r=Request{};strcpy(r.echo,"client-\\\"1");
    assert(completion(id,r,s.utc,topic,sizeof topic,payload,sizeof payload));
    assert(strstr(payload,"client-\\\\\\\"1"));
    // SOC does not depend on an optional BMS voltage. When supplied, voltage
    // supports 12/24/48 V packs instead of the old hard-coded 48 V range.
    s.external_voltage_valid=false;
    for(int i=0;i<topic_count();i++) {
        assert(notification(i,id,s,topic,sizeof topic,payload,sizeof payload));
        if(strstr(topic,"/system/0/Dc/Battery/Soc"))assert(!strcmp(payload,"{\"value\":21.000}"));
        if(strstr(topic,"/system/0/Dc/Battery/Voltage"))assert(!strcmp(payload,"{\"value\":52.000}"));
        if(strstr(topic,"/battery/0/Dc/0/Voltage"))assert(!strcmp(payload,"{\"value\":null}"));
    }
    s.external_voltage_valid=true;s.external_voltage=14.2f;
    for(int i=0;i<topic_count();i++) {
        assert(notification(i,id,s,topic,sizeof topic,payload,sizeof payload));
        if(strstr(topic,"/battery/0/Dc/0/Voltage"))assert(!strcmp(payload,"{\"value\":14.200}"));
    }
    // External battery expiry uses actual Multi SOC for system SOC, without claiming the
    // stale external battery has a reading. Loss of VE.Bus invalidates its channels.
    s.external_soc_valid=false;s.external_voltage_valid=false;
    for(int i=0;i<topic_count();i++) {
        assert(notification(i,id,s,topic,sizeof topic,payload,sizeof payload));
        if(strstr(topic,"/system/0/Dc/Battery/Soc"))assert(!strcmp(payload,"{\"value\":19.500}"));
        if(strstr(topic,"/battery/0/Soc"))assert(!strcmp(payload,"{\"value\":null}"));
    }
    s.now+=6000000;
    for(int i=0;i<topic_count();i++) {
        assert(notification(i,id,s,topic,sizeof topic,payload,sizeof payload));
        if(strstr(topic,"/vebus/288/Ac/")||strstr(topic,"/vebus/288/Dc/"))assert(!strcmp(payload,"{\"value\":null}"));
    }
    s.now=9000000;s.bus.mains_v=reading(0);s.bus.bat_a.v=NAN;
    for(int i=0;i<topic_count();i++) {
        assert(notification(i,id,s,topic,sizeof topic,payload,sizeof payload));
        if(strstr(topic,"/ActiveIn/L1/F")||strstr(topic,"/Dc/0/Power"))assert(!strcmp(payload,"{\"value\":null}"));
    }
    puts("GX telemetry: 17 sensors, power signs, source freshness/fallback, fixed read-only paths, bounds and echo escaping passed");
}
