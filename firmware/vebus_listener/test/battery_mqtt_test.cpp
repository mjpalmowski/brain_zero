#include "battery_mqtt.h"
#include "ha_mqtt.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>

using namespace battery_bms;
namespace {
Snapshot full() {
    Snapshot s; s.connected = s.complete = true;
    s.valid = SOC|VOLTAGE|CURRENT|CELL_EXTREMA|CELL_VALUES|CAPACITY|REMAINING|SOH|TEMPERATURES|
        MOS_TEMPERATURE|PACK_TEMPERATURE|LIMITS|PERMISSIONS|ALARMS|BALANCING|CYCLES|IDENTITY;
    s.soc = 56; s.voltage = 52.49f; s.current = -12; s.capacity_ah = 230; s.remaining_ah = 128.8f; s.soh = 98;
    s.min_cell_v = 3.27f; s.max_cell_v = 3.3f; s.delta_cell_v = .03f;
    s.temp_min_c = -5; s.temp_max_c = 27.3f; s.temp_mos_c = 31.2f; s.temperature_c = 29.2f;
    s.charge_voltage_limit_v = 57.7f; s.charge_current_limit_a = 145; s.discharge_current_limit_a = 155;
    s.charge_allowed = true; s.discharge_allowed = false; s.balancing = true; s.alarms = 0x800; s.warnings = 0x10;
    s.cycles = 481; s.cell_count = MAX_CELLS; for (auto &v : s.cell_v) v = 3.28f;
    std::memcpy(s.model, "JK \"mock\"\\test", 15); std::memcpy(s.hardware, "10.XG", 6); std::memcpy(s.firmware, "10.07", 6);
    std::memcpy(s.protocol, "JK02_24S", 9); s.sample_age_s = .3f; s.soc_age_s = .1f; s.cell_age_s = .2f;
    s.permissions_age_s = .2f; s.alarms_age_s = .2f; s.limits_age_s = .2f;
    return s;
}
void emit(const char *name,const Snapshot &s) {
    char data[2048]; const size_t n = battery_mqtt::state_json(s,data,sizeof data); assert(n && n<sizeof data);
    std::printf("{\"case\":\"%s\",\"state\":%s}\n",name,data);
}
}
int main(int argc,char **argv) {
    const bool output = argc>1 && !std::strcmp(argv[1],"--emit");
    ha::Settings settings; ha::Ids ids; std::strcpy(settings.base,"test/brain_zero");
    std::strcpy(ids.node,"brain_zero_mock");std::strcpy(ids.url,"http://brain-zero.local/");
    Snapshot s=full();char state[2048];
    const size_t length=battery_mqtt::state_json(s,state,sizeof state);assert(length && length<sizeof state);
    assert(std::strstr(state,"\"current\":-12.000") && std::strstr(state,"\"charge_allowed\":true"));
    assert(std::strstr(state,"\"discharge_allowed\":false"));
    char tiny[10];assert(!battery_mqtt::state_json(s,tiny,sizeof tiny) && !tiny[0]);
    assert(!battery_mqtt::state_json(s,nullptr,100)); assert(!battery_mqtt::state_json(s,state,0));
    if(output)emit("full",s);
    s.valid &= ~(SOC|PERMISSIONS|LIMITS); s.complete=false;
    battery_mqtt::state_json(s,state,sizeof state);
    assert(std::strstr(state,"\"online\":true") && std::strstr(state,"\"complete\":false"));
    assert(std::strstr(state,"\"soc\":null") && std::strstr(state,"\"charge_allowed\":null"));
    if(output)emit("partial",s);
    s=full();s.connected=false;battery_mqtt::state_json(s,state,sizeof state);
    assert(std::strstr(state,"\"online\":false") && std::strstr(state,"\"soc\":null") && std::strstr(state,"\"cell_v\":null"));
    if(output)emit("disconnected",s);
    s=full();s.valid=IDENTITY;battery_mqtt::state_json(s,state,sizeof state);assert(std::strstr(state,"\"online\":false"));
    if(output)emit("identity_only",s);
    s=full();s.current=NAN;s.temp_min_c=NAN;s.cell_v[5]=NAN;
    battery_mqtt::state_json(s,state,sizeof state);
    assert(std::strstr(state,"\"current\":null") && std::strstr(state,"\"power\":null") && std::strstr(state,"\"cell_v\":null"));
    assert(!std::strstr(state,"nan") && !std::strstr(state,"NaN"));
    if(output)emit("invalid_numeric",s);
    s=full();s.cell_count=255;battery_mqtt::state_json(s,state,sizeof state);assert(std::strstr(state,"\"cell_count\":null"));
    if(output)emit("invalid_cells",s);
    char topic[256],payload[2048];
    assert(battery_mqtt::entity_count()==93);
    for(int i=0;i<battery_mqtt::entity_count();++i) {
        assert(battery_mqtt::discovery(i,settings,ids,false,topic,sizeof topic,payload,sizeof payload));
        assert(std::strlen(payload)<sizeof payload);
        if(output)std::printf("{\"discovery\":%d,\"topic\":\"%s\",\"payload\":%s}\n",i,topic,payload);
        assert(battery_mqtt::discovery(i,settings,ids,true,topic,sizeof topic,payload,sizeof payload) && !payload[0]);
    }
    assert(!battery_mqtt::discovery(-1,settings,ids,false,topic,sizeof topic,payload,sizeof payload));
    assert(!battery_mqtt::discovery(battery_mqtt::entity_count(),settings,ids,false,topic,sizeof topic,payload,sizeof payload));
    assert(!battery_mqtt::discovery(0,settings,ids,false,topic,1,payload,sizeof payload));
    assert(!battery_mqtt::discovery(0,settings,ids,false,topic,sizeof topic,payload,1));
    assert(!battery_mqtt::discovery(0,settings,ids,false,nullptr,10,payload,sizeof payload));
    assert(!battery_mqtt::discovery(0,settings,ids,false,topic,sizeof topic,nullptr,10));
    if(!output)std::puts("Battery MQTT field validity, partial availability, JSON bounds and93 discovery messages passed");
}
