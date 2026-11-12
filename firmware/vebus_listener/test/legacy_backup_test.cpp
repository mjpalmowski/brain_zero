// Portable integration test: actual Requester frames and RVSC codec, entirely
// synthetic identities/settings/records. Only the documented eight-byte legacy
// version prefix is observed data. No recorded installation exports are used.
#include "requester.h"
#include "rvsc.h"
#include "vebus_state.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>

using vebus::Requester;
#define CHECK(c) do { if (!(c)) throw std::runtime_error(#c); } while (0)

namespace {
constexpr int64_t CYCLE = 19967;
constexpr uint8_t LEGACY[] = {0x6B,0x1B,0x28,0,0x08,0x0D,0,0};
struct Fixture {
    uint32_t firmware = 2628459;
    uint8_t model = 47, version[13] = {};
    size_t version_bytes = 8;
    bool drop_grid = false, drop_assistants = false, drop_setting = false, drop_block = false;
    Fixture() {
        std::memcpy(version, LEGACY, sizeof LEGACY);
        version[8] = 0xA5; version[9] = 0x5A; version[10] = 0xCC;
    }
    void identity(uint32_t value) {
        firmware = value;
        for (unsigned i=0;i<4;++i) version[i]=uint8_t(value>>(8*i));
    }
};

class Bus {
public:
    Requester requester;
    Fixture fixture;
    int64_t now = 100000000;
    unsigned grid_queries = 0, writes = 0;
    explicit Bus(Fixture f) : fixture(f) {
        requester.control(true); sync(now-4000000); sync(now-CYCLE);
    }
    void read() {
        CHECK(requester.read_settings(now));
        CHECK(requester.settings().version_bytes==0);
        CHECK(requester.settings().grid_availability==Requester::GridAvailability::UNKNOWN);
        for (int cycle=0;cycle<6000 && !requester.settings().done;++cycle) tick();
        CHECK(requester.settings().done && !requester.settings().active && writes==0);
    }
    void poll(unsigned cycles) { while(cycles--) tick(); }
private:
    uint8_t number = 0, later[12] = {};
    bool pending = false;
    void heard(const uint8_t *p,size_t n,int64_t at,uint8_t type=0xFE) {
        uint8_t frame[64];
        size_t len=vebus::build_frame(0x83,0x83,type,number++,p,n,frame,sizeof frame);
        CHECK(len); requester.heard(frame,len,at);
    }
    void sync(int64_t at) {
        const uint8_t p[]={0x55,0xA9,0,0}; heard(p,sizeof p,at,0xFD);
    }
    void reply(const uint8_t *p,size_t len) {
        const int64_t at=now+16080;
        if(p[0]==0x21 && len>=6) {
            if(p[5]==5) {
                const uint8_t q[]={0x20,1,2,3,4,fixture.model,1,1,1,0,0,0,0,0xF0,0};
                heard(q,sizeof q,at);
            } else if(p[5]==8) {
                const uint8_t q[]={0x20,1,2,3,4,uint8_t(p[6]==0xFF?0x0C:8),0,0,0,0,0,0,0,0};
                heard(q,sizeof q,at);
            } else CHECK(p[5]==0x0E);
            return;
        }
        CHECK(p[0]==0 && len>=3);
        uint8_t q[24]={0,p[1]}; size_t n=3;
        switch(p[2]) {
        case 0x3B: {
            const char serial[]="BZSIM000000"; q[2]=0x9D;
            std::memcpy(q+3,serial,sizeof serial-1);n+=sizeof serial-1;break;
        }
        case 5: q[2]=0x82;std::memcpy(q+3,fixture.version,fixture.version_bytes);n+=fixture.version_bytes;break;
        case 0x35: {
            const unsigned id=p[3]|unsigned(p[4])<<8;
            if(fixture.drop_setting && id==40)return;
            q[2]=0x89;
            if(id!=13) {
                q[3]=1;q[7]=uint8_t(id);q[8]=uint8_t(id>>8);
                q[11]=q[12]=0xFF;q[13]=0;n=14;
            }
            break;
        }
        case 0x31: q[2]=0x86;q[3]=p[3];q[4]=p[4];n=5;break;
        case 0x3C:
            if(fixture.drop_block)return;
            q[2]=0x9E;q[3]=p[3];n=12;
            std::memcpy(later,q,n);later[2]=0x9F;pending=true;break;
        case 0x57:
        case 0x5F: {
            const bool grid=p[2]==0x57;
            if(grid){++grid_queries;if(fixture.drop_grid)return;}
            else if(fixture.drop_assistants)return;
            if(p[4]>=(grid?1:64)){q[2]=0x80;q[3]=2;n=4;}
            else if(grid){q[2]=0xA3;n=14;for(unsigned i=0;i<11;++i){unsigned off=(p[3]-1)*11+i;q[3+i]=off<64?uint8_t(off):0xFF;}}
            else q[2]=0x96;  // 64 explicitly blank synthetic Assistant records
            break;
        }
        case 0x30: q[2]=0x85;n=3+2*(len-3);break;
        case 0x38: q[2]=0x99;n=15;break;
        case 0x0E: CHECK(p[3]==0);q[2]=0x94;q[3]=9;q[4]=3;n=5;break;
        default: ++writes;throw std::runtime_error("Non-read command during backup");
        }
        heard(q,n,at);
    }
    void tick() {
        sync(now);
        if(pending){heard(later,sizeof later,now+16080);pending=false;}
        else {
            uint8_t frame[64],payload[24]={};size_t n=0;int64_t early=0,late=0;
            if(requester.due(now+700,frame,sizeof frame,&n,&early,&late)) {
                requester.sent(frame,n,early,early+int64_t(n)*Requester::BYTE_US+Requester::ADDRESS_PAUSE_US);
                ++number;requester.heard(frame,n,early);
                size_t len=vebus::payload_of(frame,n,payload,sizeof payload);CHECK(len);reply(payload,len);
            }
        }
        requester.tick(now+19000);now+=CYCLE;char log[1100];requester.take_report(log,sizeof log);
    }
};

void export_case(const char *name,Fixture fixture,bool omitted,bool ready) {
    Bus bus(fixture);bus.read();const auto &r=bus.requester.settings();
    CHECK(r.firmware==fixture.firmware && r.unit_info[0]==fixture.model);
    CHECK(r.version_bytes==fixture.version_bytes);
    CHECK(r.version_ok==(fixture.version_bytes>=10));
    CHECK(!std::memcmp(r.version,fixture.version,std::min(sizeof r.version,fixture.version_bytes)));
    CHECK((r.grid_availability==Requester::GridAvailability::LEGACY_OMITTED)==omitted);
    if(omitted)CHECK(bus.grid_queries==0 && r.grid.count==0 && !r.grid.ended);
    else CHECK(bus.grid_queries>0);
    const char *why=nullptr;CHECK(rvsc::ready(r,&why)==ready);
    uint8_t data[rvsc::MAX_SIZE];size_t bytes=rvsc::build(r,1767225800,data,sizeof data);
    CHECK((bytes!=0)==ready);
    if(ready) {
        rvsc::File f;char error[180];CHECK(rvsc::parse(data,bytes,f,error,sizeof error));
        CHECK(f.schema_version==(omitted?2u:4u) && f.unit_version==(omitted?2u:3u));
        CHECK(f.version_bytes==fixture.version_bytes && f.grid_present==!omitted);
        CHECK(!std::memcmp(f.version,fixture.version,fixture.version_bytes));
        CHECK(f.settings[40]==40 && f.assistant_bytes==4096);
        if(omitted)CHECK(f.grid_len==0);
    }
    if(fixture.firmware==2628459) {
        CHECK(bus.requester.firmware_gate(bus.now));
        CHECK(bus.requester.ess_firmware_gate(bus.now));
        CHECK(bus.requester.settings_write_firmware_gate(bus.now));
        Requester::WritePlan plan;
        plan.settings=1;plan.setting_id[0]=2;plan.setting_value[0]=1;plan.read_started=r.started;
        std::memcpy(plan.unit_id,r.unit_id,sizeof plan.unit_id);
        CHECK(!bus.requester.start_write(bus.now,plan,&why));
        bus.requester.target(true,100,0);
        bus.poll(50);
        CHECK(bus.writes==0 && !bus.requester.write_job().active);
        CHECK(bus.requester.ctl_stats().writes==0 && bus.requester.ctl_stats().flag_writes==0);
    }
    std::printf("passed: %s\n",name);
}
}

int main() {
    try {
        Fixture f;export_case("legacy eight bytes",f,true,true);
        f.version_bytes=10;export_case("legacy ten bytes preserved",f,true,true);
        f=Fixture();f.version_bytes=7;export_case("legacy seven bytes refused",f,false,false);
        f.version_bytes=9;export_case("legacy nine bytes refused",f,false,false);
        f.version_bytes=11;export_case("legacy eleven bytes refused",f,false,false);
        f=Fixture();f.identity(2628458);export_case("other firmware cannot omit grid",f,false,false);
        f=Fixture();f.model=46;export_case("other model cannot omit grid",f,false,false);
        f=Fixture();f.version[5]^=1;export_case("different prefix cannot omit grid",f,false,false);
        f=Fixture();f.version_bytes=10;f.identity(2628458);f.drop_grid=true;
        export_case("other full firmware cannot downgrade a grid timeout",f,false,false);
        f=Fixture();f.version_bytes=10;f.model=46;f.drop_grid=true;
        export_case("other full model cannot downgrade a grid timeout",f,false,false);
        f=Fixture();f.version_bytes=10;f.version[5]^=1;f.drop_grid=true;
        export_case("other full prefix cannot downgrade a grid timeout",f,false,false);
        f=Fixture();f.drop_assistants=true;export_case("legacy missing Assistants refused",f,true,false);
        f=Fixture();f.drop_setting=true;export_case("legacy missing setting refused",f,true,false);
        f=Fixture();f.drop_block=true;export_case("legacy missing flag block refused",f,true,false);
        f=Fixture();f.identity(2780563);f.version_bytes=10;f.model=2;
        export_case("modern unqualified full backup",f,false,true);
        f.drop_grid=true;export_case("modern grid timeout is failure",f,false,false);
        Bus reused{Fixture()};reused.read();reused.fixture=f;reused.fixture.drop_grid=false;
        reused.read();CHECK(reused.requester.settings().version_bytes==10);
        CHECK(reused.requester.settings().grid_availability==Requester::GridAvailability::AVAILABLE);
        CHECK(reused.requester.settings().grid.ended && reused.grid_queries>0);
        std::puts("passed: new read resets legacy omission state");
    } catch(const std::exception &e) {
        std::fprintf(stderr,"FAILED: %s\n",e.what());return 1;
    }
    std::puts("Legacy backup requester integration tests passed.");return 0;
}
