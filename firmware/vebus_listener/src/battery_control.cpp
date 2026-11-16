#include "battery_control.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace battery_control {
namespace {
bool fresh(int64_t at,int64_t now) { return at>=0 && now>=at && now-at<battery_bms::FRESH_US; }
int64_t observed(int64_t now,float age) {
    if(!std::isfinite(age) || age<0 || age>=10 || now<0)return -1;
    const int64_t delta=int64_t(std::ceil(double(age)*1000000.0))+1000;
    return now>=delta?now-delta:-1;
}
bool failure(char *why,size_t cap,const char *message) { if(why && cap)snprintf(why,cap,"%s",message);return false; }
bool finite_range(float value,float low,float high) {return std::isfinite(value) && value>=low && value<=high;}
bool extrema_valid(float low,float high,float delta) {
    if(!finite_range(low,.5f,5) || !finite_range(high,low,5) || !finite_range(delta,0,4.5f))return false;
    const long span=std::lround(double(high)*1000.0)-std::lround(double(low)*1000.0);
    const long reported=std::lround(double(delta)*1000.0);
    return std::labs(span-reported)<=1;
}
bool pack_guard_valid(const battery_bms::Snapshot &p) {
    using namespace battery_bms;
    return p.connected && p.has(SOC|CELL_EXTREMA|PERMISSIONS|ALARMS) &&
        finite_range(p.soc,0,100) && extrema_valid(p.min_cell_v,p.max_cell_v,p.delta_cell_v) &&
        finite_range(p.soc_age_s,0,10) && finite_range(p.cell_age_s,0,10) &&
        finite_range(p.permissions_age_s,0,10) && finite_range(p.alarms_age_s,0,10);
}
}
bool valid(const Settings &s,char *why,size_t cap) {
    if(why && cap)*why=0;
    if(s.version!=1 || unsigned(s.soc_source)>unsigned(SocSource::BANK))return failure(why,cap,"Invalid battery control settings");
    if(s.max_soc>100 || s.max_cell_mv>4500 || s.max_delta_mv>1000 || s.limited_charge_w>5000)
        return failure(why,cap,"Battery charge limit is outside its range");
    if((s.max_cell_mv && s.max_cell_mv<2000) || (s.max_delta_mv && s.max_delta_mv<5))
        return failure(why,cap,"Cell voltage must be 2.000–4.500 V; cell difference 5–1000 mV");
    if(s.charge_guard && (!s.max_soc || !s.max_cell_mv || !s.max_delta_mv))
        return failure(why,cap,"Set SOC, maximum cell voltage and cell difference before enabling charge limits");
    return true;
}
Inputs inputs(const battery_bms::Snapshot &a,const battery_bms::Snapshot &b,
              const battery_bms::Snapshot &bank,int64_t now) {
    using namespace battery_bms;
    Inputs in;const Snapshot *p[]={&a,&b,&bank};
    for(unsigned n=0;n<3;++n) {
        if(p[n]->connected && p[n]->has(SOC)) {in.soc[n]=p[n]->soc;in.soc_at[n]=observed(now,p[n]->soc_age_s);}
        if(p[n]->connected && p[n]->has(VOLTAGE)) {in.voltage[n]=p[n]->voltage;in.voltage_at[n]=observed(now,p[n]->sample_age_s);}
    }
    if(pack_guard_valid(a) && pack_guard_valid(b)) {
        const float age=std::max(std::max(a.soc_age_s,b.soc_age_s),
            std::max(std::max(a.cell_age_s,b.cell_age_s),
                std::max(std::max(a.permissions_age_s,b.permissions_age_s),std::max(a.alarms_age_s,b.alarms_age_s))));
        in.guard_at=observed(now,age);
        in.highest_soc=std::max(a.soc,b.soc);
        in.min_cell_v=std::min(a.min_cell_v,b.min_cell_v);in.max_cell_v=std::max(a.max_cell_v,b.max_cell_v);
        in.delta_cell_v=in.max_cell_v-in.min_cell_v;
        in.charge_allowed=a.charge_allowed && b.charge_allowed;
        in.protections_clear=!a.alarms && !b.alarms;
    }
    return in;
}
bool selected_soc(const Settings &s,const Inputs &in,int64_t now,float &soc) {
    const unsigned source=unsigned(s.soc_source);
    if(!valid(s) || source<1 || source>3 || !fresh(in.soc_at[source-1],now))return false;
    const float value=in.soc[source-1];
    if(!finite_range(value,0,100))return false;
    soc=value;return true;
}
bool selected_voltage(const Settings &s,const Inputs &in,int64_t now,float &voltage) {
    const unsigned source=unsigned(s.soc_source);
    if(!valid(s) || source<1 || source>3 || !fresh(in.voltage_at[source-1],now))return false;
    const float value=in.voltage[source-1];
    if(!finite_range(value,3,160))return false;
    voltage=value;return true;
}
View Guard::update(const Settings &s,const Inputs &in,int64_t now) {
    View out;out.enabled=s.charge_guard;
    if(!s.charge_guard) {latched_=0;return out;}
    out.data_valid=valid(s) && fresh(in.guard_at,now) && finite_range(in.highest_soc,0,100) &&
        extrema_valid(in.min_cell_v,in.max_cell_v,in.delta_cell_v);
    if(!out.data_valid) {out.limited=true;out.charge_cap_w=0;out.reason="Battery data unavailable: charging paused";return out;}
    if(!in.protections_clear) {out.limited=true;out.charge_cap_w=0;out.reason="A battery protection is active";return out;}
    if(!in.charge_allowed) {out.limited=true;out.charge_cap_w=0;out.reason="A battery does not allow charging";return out;}
    // Source cell voltages have1mV resolution. Compare integer millivolts so
    // 3.285-3.280 does not miss a5mV threshold through float rounding.
    const long highest_mv=std::lround(double(in.max_cell_v)*1000.0);
    const long lowest_mv=std::lround(double(in.min_cell_v)*1000.0);
    const long delta_mv=std::max(std::lround(double(in.delta_cell_v)*1000.0),highest_mv-lowest_mv);
    const long delta_hysteresis=std::min(5u,(unsigned(s.max_delta_mv)+4u)/5u);
    if(in.highest_soc>=s.max_soc)latched_|=1;
    else if(in.highest_soc<=float(s.max_soc)-1)latched_&=~1u;
    if(highest_mv>=s.max_cell_mv)latched_|=2;
    else if(highest_mv<=long(s.max_cell_mv)-10)latched_&=~2u;
    if(delta_mv>=s.max_delta_mv)latched_|=4;
    else if(delta_mv<=long(s.max_delta_mv)-delta_hysteresis)latched_&=~4u;
    out.limited=latched_!=0;out.charge_cap_w=out.limited?s.limited_charge_w:-1;
    out.reason=(latched_&2)?"Maximum cell voltage reached":(latched_&4)?"Cell voltage difference reached":
        (latched_&1)?"Battery SOC limit reached":"Within battery charge limits";
    return out;
}
}
