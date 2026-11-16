#include "battery_mqtt.h"
#include "ha_mqtt.h"
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace battery_mqtt {
namespace {
struct Json {
    char *out; size_t cap, n = 0; bool good = true;
    Json(char *p, size_t c) : out(p), cap(p?c:0) { if(cap) *p=0; }
    void add(const char *format, ...) {
        if (!good || n >= cap) { good=false; return; }
        va_list args; va_start(args,format);
        const int k=vsnprintf(out+n,cap-n,format,args); va_end(args);
        if(k<0 || size_t(k)>=cap-n) { good=false; return; } n+=size_t(k);
    }
    void str(const char *s,size_t limit=SIZE_MAX) {
        add("\""); for(size_t n=0;s && n<limit && s[n];++n) {
            const unsigned char c=s[n];
            if(c=='\"' || c=='\\') add("\\%c",c);
            else if(c<32)add("\\u%04x",unsigned(c));
            else if(c<128)add("%c",c);
            else {
                const unsigned bytes=(c>=0xc2 && c<=0xdf)?2:(c>=0xe0 && c<=0xef)?3:(c>=0xf0 && c<=0xf4)?4:0;
                uint32_t code=bytes?uint32_t(c&((1u<<(7u-bytes))-1u)):0;
                bool valid=bytes && n+bytes<=limit;
                for(unsigned k=1;valid && k<bytes;++k) {
                    const unsigned char next=s[n+k];
                    if(!next || (next&0xc0)!=0x80)valid=false;
                    else code=(code<<6)|(next&0x3f);
                }
                valid=valid && code>=((bytes==2)?0x80u:(bytes==3)?0x800u:0x10000u) &&
                    code<=0x10ffff && !(code>=0xd800 && code<=0xdfff);
                if(valid) {
                    if(code<=0xffff)add("\\u%04x",unsigned(code));
                    else {code-=0x10000;add("\\u%04x\\u%04x",unsigned(0xd800+(code>>10)),unsigned(0xdc00+(code&0x3ff)));}
                    n+=bytes-1;
                } else add("\\ufffd");
            }
        } add("\"");
    }
    void number(const char *key, bool valid, float value, unsigned decimals=3) {
        add(",\"%s\":",key); if(valid && std::isfinite(value)) add("%.*f",int(decimals),double(value)); else add("null");
    }
    void flag(const char *key,bool valid,bool value) {
        add(",\"%s\":%s",key,valid?(value?"true":"false"):"null");
    }
    size_t finish() { if(!good) { if(cap) *out=0; return 0; } return n; }
};
struct Metric { const char *key,*name,*unit,*device; bool binary; };
const Metric metrics[] = {
    {"soc","State of charge","%","battery",false},
    {"voltage","Battery voltage","V","voltage",false},
    {"current","Battery current","A","current",false},
    {"power","Battery power","W","power",false},
    {"min_cell_v","Minimum cell voltage","V","voltage",false},
    {"max_cell_v","Maximum cell voltage","V","voltage",false},
    {"delta_cell_v","Cell voltage difference","V","voltage",false},
    {"capacity_ah","Battery capacity","Ah","",false},
    {"remaining_ah","Remaining capacity","Ah","",false},
    {"soh","State of health","%","",false},
    {"temp_min_c","Minimum battery temperature","°C","temperature",false},
    {"temp_max_c","Maximum battery temperature","°C","temperature",false},
    {"temp_mos_c","MOSFET temperature","°C","temperature",false},
    {"temperature_c","Battery temperature","°C","temperature",false},
    {"charge_voltage_limit_v","Charge voltage limit","V","voltage",false},
    {"charge_current_limit_a","Charge current limit","A","current",false},
    {"discharge_current_limit_a","Discharge current limit","A","current",false},
    {"charge_allowed","Charging allowed","","",true},
    {"discharge_allowed","Discharging allowed","","",true},
    {"balancing","Balancing","","",true},
    {"alarms","Alarm bitmask","","",false},
    {"warnings","Warning bitmask","","",false},
    {"sample_age_s","Data age","s","duration",false},
};
constexpr int METRICS=sizeof(metrics)/sizeof(metrics[0]);
}

const char *peer_name(unsigned peer) { return peer==0?"jk":peer==1?"pace":"bank"; }

size_t state_json(const battery_bms::Snapshot &s,char *out,size_t cap) {
    using namespace battery_bms;
    uint32_t valid=s.connected?s.valid:0;
    const auto finite=[](float v){return std::isfinite(v);};
    if(!finite(s.soc))valid&=~SOC;
    if(!finite(s.voltage))valid&=~VOLTAGE;
    if(!finite(s.current))valid&=~CURRENT;
    if(!finite(s.capacity_ah))valid&=~CAPACITY;
    if(!finite(s.remaining_ah))valid&=~REMAINING;
    if(!finite(s.soh))valid&=~SOH;
    if(!finite(s.min_cell_v) || !finite(s.max_cell_v) || !finite(s.delta_cell_v))valid&=~CELL_EXTREMA;
    if(!finite(s.temp_min_c) || !finite(s.temp_max_c))valid&=~TEMPERATURES;
    if(!finite(s.temp_mos_c))valid&=~MOS_TEMPERATURE;
    if(!finite(s.temperature_c))valid&=~PACK_TEMPERATURE;
    if(!finite(s.charge_voltage_limit_v) || !finite(s.charge_current_limit_a) || !finite(s.discharge_current_limit_a))valid&=~LIMITS;
    if(!s.cell_count || s.cell_count>MAX_CELLS)valid&=~CELL_VALUES;
    else for(size_t i=0;i<s.cell_count;++i)if(!finite(s.cell_v[i]))valid&=~CELL_VALUES;
    const auto has=[valid](uint32_t fields){return (valid&fields)==fields;};
    const bool online=s.connected && (valid&~IDENTITY)!=0;
    const bool complete=has(SOC|VOLTAGE|CURRENT);
    Json j(out,cap);
    j.add("{\"schema\":1,\"online\":%s,\"connected\":%s,\"complete\":%s,\"valid\":%u,\"model\":",
          online?"true":"false",s.connected?"true":"false",complete?"true":"false",unsigned(valid));
    j.str(s.model,sizeof s.model);j.add(",\"hardware\":");j.str(s.hardware,sizeof s.hardware);j.add(",\"firmware\":");j.str(s.firmware,sizeof s.firmware);
    j.add(",\"protocol\":");j.str(s.protocol,sizeof s.protocol);
    j.number("soc",has(SOC),s.soc,2);j.number("voltage",has(VOLTAGE),s.voltage);
    j.number("current",has(CURRENT),s.current);
    j.number("power",has(VOLTAGE|CURRENT),s.voltage*s.current,1);
    j.number("capacity_ah",has(CAPACITY),s.capacity_ah,2);j.number("remaining_ah",has(REMAINING),s.remaining_ah,2);
    j.number("soh",has(SOH),s.soh,1);
    j.number("min_cell_v",has(CELL_EXTREMA),s.min_cell_v);j.number("max_cell_v",has(CELL_EXTREMA),s.max_cell_v);
    j.number("delta_cell_v",has(CELL_EXTREMA),s.delta_cell_v);
    j.number("temp_min_c",has(TEMPERATURES),s.temp_min_c,1);j.number("temp_max_c",has(TEMPERATURES),s.temp_max_c,1);
    j.number("temp_mos_c",has(MOS_TEMPERATURE),s.temp_mos_c,1);
    j.number("temperature_c",has(PACK_TEMPERATURE),s.temperature_c,1);
    j.number("charge_voltage_limit_v",has(LIMITS),s.charge_voltage_limit_v,2);
    j.number("charge_current_limit_a",has(LIMITS),s.charge_current_limit_a,1);
    j.number("discharge_current_limit_a",has(LIMITS),s.discharge_current_limit_a,1);
    j.flag("charge_allowed",has(PERMISSIONS),s.charge_allowed);j.flag("discharge_allowed",has(PERMISSIONS),s.discharge_allowed);
    j.flag("force_charge",has(PERMISSIONS),s.force_charge);j.flag("full_charge_requested",has(PERMISSIONS),s.full_charge_requested);
    j.flag("balancing",has(BALANCING),s.balancing);
    j.add(",\"alarms\":");if(has(ALARMS))j.add("%u",unsigned(s.alarms));else j.add("null");
    j.add(",\"warnings\":");if(has(ALARMS))j.add("%u",unsigned(s.warnings));else j.add("null");
    j.add(",\"cycles\":");if(has(CYCLES))j.add("%u",unsigned(s.cycles));else j.add("null");
    j.number("sample_age_s",s.sample_age_s>=0,s.sample_age_s);
    j.number("soc_age_s",s.soc_age_s>=0,s.soc_age_s);j.number("cell_age_s",s.cell_age_s>=0,s.cell_age_s);
    j.number("limits_age_s",s.limits_age_s>=0,s.limits_age_s);j.number("permissions_age_s",s.permissions_age_s>=0,s.permissions_age_s);
    j.number("alarms_age_s",s.alarms_age_s>=0,s.alarms_age_s);
    j.add(",\"cell_count\":");if(has(CELL_VALUES))j.add("%u",s.cell_count);else j.add("null");
    j.add(",\"cell_v\":");if(has(CELL_VALUES)) {
        j.add("[");for(size_t i=0;i<s.cell_count && i<MAX_CELLS;++i)j.add("%s%.3f",i?",":"",double(s.cell_v[i]));j.add("]");
    } else j.add("null");
    j.add("}");return j.finish();
}

int entity_count() { return 3*METRICS+int(battery_bms::MAX_CELLS); }
bool discovery(int index,const ha::Settings &cfg,const ha::Ids &ids,bool remove,
               char *topic,size_t tc,char *out,size_t cap) {
    if(index<0 || index>=entity_count())return false;
    const bool cell=index>=3*METRICS;
    const int peer=cell?0:index/METRICS;
    const Metric &m=metrics[cell?0:index%METRICS];
    if(!topic || !tc || !out || !cap)return false;
    *topic=0;*out=0;
    char key[48],name[72],value[160],available[144];
    int value_size=0,available_size=0;
    if(cell) {
        const int n=index-3*METRICS;
        snprintf(key,sizeof key,"cell_%02d",n+1);snprintf(name,sizeof name,"Cell %d voltage",n+1);
        value_size=snprintf(value,sizeof value,"{{(value_json.cell_v or [])[ %d ]|default(none)}}",n);
        available_size=snprintf(available,sizeof available,"{{'online' if (value_json.cell_v or [])|length>%d else 'offline'}}",n);
    } else {
        snprintf(key,sizeof key,"%s",m.key);snprintf(name,sizeof name,"%s",m.name);
        if(m.binary)value_size=snprintf(value,sizeof value,"{%% set v=value_json.%s %%}{{'ON' if v==true else 'OFF' if v==false else none}}",m.key);
        else value_size=snprintf(value,sizeof value,"{{value_json.%s}}",m.key);
        available_size=snprintf(available,sizeof available,"{{'online' if value_json.%s is not none else 'offline'}}",m.key);
    }
    if(value_size<0 || size_t(value_size)>=sizeof value || available_size<0 || size_t(available_size)>=sizeof available)return false;
    const char *p=peer_name(peer),*component=!cell && m.binary?"binary_sensor":"sensor";
    const int n=snprintf(topic,tc,"%s/%s/%s/bms_%s_%s/config",cfg.prefix,component,ids.node,p,key);
    if(n<0 || size_t(n)>=tc || !cap)return false;
    if(remove){*out=0;return true;}
    Json j(out,cap);
    j.add("{\"name\":");j.str(name);j.add(",\"unique_id\":\"%s_bms_%s_%s\",\"state_topic\":\"%s/bms/%s\",\"value_template\":",ids.node,p,key,cfg.base,p);j.str(value);
    if(!cell && m.binary)j.add(",\"payload_on\":\"ON\",\"payload_off\":\"OFF\"");
    else {
        const char *unit=cell?"V":m.unit,*device=cell?"voltage":m.device;
        if(*unit){j.add(",\"unit_of_measurement\":");j.str(unit);}
        if(*device){j.add(",\"device_class\":");j.str(device);}
        if(strcmp(m.key,"alarms") && strcmp(m.key,"warnings"))j.add(",\"state_class\":\"measurement\"");
    }
    if(cell || !strcmp(m.key,"alarms") || !strcmp(m.key,"warnings") || !strcmp(m.key,"sample_age_s"))
        j.add(",\"entity_category\":\"diagnostic\",\"enabled_by_default\":false");
    j.add(",\"expire_after\":%u,\"availability\":[{\"topic\":\"%s/status\"},{\"topic\":\"%s/bms/%s\",\"value_template\":\"{{'online' if value_json.online else 'offline'}}\"},{\"topic\":\"%s/bms/%s\",\"value_template\":",
          unsigned(cfg.interval_s)*3u+10u,cfg.base,cfg.base,p,cfg.base,p);
    j.str(available);
    j.add("}],\"availability_mode\":\"all\",\"device\":{\"identifiers\":[\"%s_bms_%s\"],\"via_device\":\"%s\",\"name\":\"brain_zero %s battery\",\"manufacturer\":\"brain_zero\",\"model\":\"%s\",\"configuration_url\":\"%ssettings/battery\"}}",
          ids.node,p,ids.node,peer==0?"JK":peer==1?"PACE":"combined",peer==2?"Parallel bank":"BLE battery monitor",ids.url);
    return j.finish()!=0;
}
} // namespace battery_mqtt
