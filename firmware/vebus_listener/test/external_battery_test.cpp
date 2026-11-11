#include "external_battery.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace {
constexpr double utc = 1794385200;
constexpr int64_t now = 50000000;
const char sample[] = "{\"soc\":86.5,\"soc_ts\":1794385199.9,\"external_current_a\":12.3,\"current_ts\":1794385199.9,\"voltage\":14.2}";
bool accept(external_battery::Feed &feed,const char *data,int64_t at=now,double clock=utc,bool retained=false,int64_t queue=now) {
    return feed.accept(data,strlen(data),retained,queue,at,clock);
}
external_battery::Feed enabled() {
    external_battery::Feed feed;feed.enabled(true);feed.connected(true);return feed;
}
}
int main() {
    static_assert(sizeof(external_battery::Feed)<128,"Input cache must stay small; no extra MQTT queue");
    {
        external_battery::Feed feed;
        assert(!accept(feed,sample));assert(!feed.view(now).soc_valid);
        feed.enabled(true);assert(!accept(feed,sample));
        feed.connected(true);assert(accept(feed,sample));
        const auto v=feed.view(now);
        assert(v.enabled&&v.connected&&v.soc_valid&&v.current_valid&&v.voltage_valid);
        assert(fabs(v.soc-86.5)<0.001&&fabs(v.external_current_a-12.3)<0.001&&fabs(v.voltage-14.2)<0.001);
        assert(v.soc_age_s>0.09&&v.soc_age_s<0.11);
        assert(feed.synchronization_started());
        assert(feed.view(now+5800000).current_valid);
        assert(!feed.view(now+6000000).current_valid);
        assert(feed.view(now+6000000).external_current_a==0);
        assert(feed.view(now+9800000).soc_valid);
        assert(!feed.view(now+10000000).soc_valid);
        assert(!feed.view(now+10000000).voltage_valid);
        assert(!feed.view(now-1).soc_valid);
    }
    // Initial source age shortens the remaining TTL; forwarding an old source
    // sample with a new MQTT receive time cannot rejuvenate it.
    {
        auto feed=enabled();assert(accept(feed,"{\"soc\":50,\"soc_ts\":1794385191}"));
        assert(feed.view(now+900000).soc_valid);assert(!feed.view(now+1000000).soc_valid);
        assert(!accept(feed,"{\"soc\":50,\"soc_ts\":1794385190}"));
        assert(!accept(feed,"{\"external_current_a\":0,\"current_ts\":1794385194}"));
    }
    {
        auto feed=enabled();assert(!accept(feed,sample,now,utc,true));
        assert(!accept(feed,sample,now,utc,false,now-3000001));
        assert(!accept(feed,sample,now,utc,false,now+1));
        assert(!accept(feed,sample,now,0));assert(!accept(feed,sample,now,NAN));
        assert(accept(feed,sample,now,utc,false,now-3000000));
        assert(!accept(feed,sample)); // QoS duplicate cannot refresh a sample.
        feed.connected(false);auto v=feed.view(now);assert(!v.soc_valid&&!v.current_valid);
        feed.connected(true);assert(!accept(feed,sample)); // nor can reconnect/replay
        feed.enabled(false);feed.enabled(true);assert(!accept(feed,sample));
        assert(accept(feed,"{\"soc\":87,\"soc_ts\":1794385201}",now+1000000,utc+1,false,now+1000000));
    }
    const char *invalid[]={
        "{}","[]","null","{\"soc\":1}","{\"soc_ts\":1794385200}",
        "{\"soc\":1,\"soc_ts\":1794385200,\"current_ts\":1794385200}",
        "{\"voltage\":52}","{\"external_current_a\":1,\"current_ts\":1794385200,\"voltage\":52}",
        "{\"soc\":-1,\"soc_ts\":1794385200}","{\"soc\":101,\"soc_ts\":1794385200}",
        "{\"soc\":1e999,\"soc_ts\":1794385200}","{\"soc\":01,\"soc_ts\":1794385200}",
        "{\"soc\":\"50\",\"soc_ts\":1794385200}","{\"soc\":true,\"soc_ts\":1794385200}",
        "{\"soc\":null,\"soc_ts\":1794385200}","{\"soc\":[],\"soc_ts\":1794385200}",
        "{\"soc\":50,\"soc\":51,\"soc_ts\":1794385200}",
        "{\"soc\":50,\"soc_ts\":1794385200,\"unknown\":1}",
        "{\"soc\":50,\"soc_ts\":1794385200,}","{\"soc\":50,\"soc_ts\":1794385200}x",
        "{\"soc\":50,\"soc_ts\":1794385206}","{\"soc\":50,\"soc_ts\":1794385190}",
        "{\"external_current_a\":-0.1,\"current_ts\":1794385200}",
        "{\"external_current_a\":200.1,\"current_ts\":1794385200}",
        "{\"soc\":50,\"soc_ts\":1794385200,\"voltage\":2.9}",
        "{\"soc\":50,\"soc_ts\":1794385200,\"voltage\":80.1}",
    };
    for(const char *data:invalid){auto feed=enabled();assert(!accept(feed,data));assert(!feed.synchronization_started());}
    {
        auto feed=enabled();assert(accept(feed,sample));
        assert(!accept(feed,"{\"soc\":50,\"soc_ts\":1794385199.9,\"external_current_a\":100,\"current_ts\":1794385200}"));
        assert(fabs(feed.view(now).external_current_a-12.3)<0.001);assert(feed.view(now).accepted==1);
        assert(accept(feed," { \"soc\":0e0,\"soc_ts\":1794385205,\"voltage\":3 } "));
        assert(accept(feed,"{\"external_current_a\":200,\"current_ts\":1794385200}"));
        assert(feed.view(now).soc==0&&feed.view(now).external_current_a==200);
        std::string oversized(257,' ');assert(!feed.accept(oversized.c_str(),oversized.size(),false,now,now,utc));
        assert(!feed.accept(nullptr,0,false,now,now,utc));
    }
    puts("External battery parsing, bounds, freshness and replay checks passed");
}
