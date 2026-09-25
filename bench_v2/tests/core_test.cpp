#include "core.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <limits>
using namespace bench;
unsigned checks=0;
#define CHECK(x) do { ++checks; if(!(x)) { std::cerr<<"FAIL line "<<__LINE__<<": "<<#x<<"\n"; std::abort(); } } while(0)
struct Fake:IO {
    bool on=false,home=false,armed=false; unsigned starts=0,stops=0; double target=0,coordinate=0;
    bool enable()override { on=true; return true; }
    void disable()override { on=false; }
    void start(double d,double)override { target=d; ++starts; }
    void stop()override { ++stops; }
    void position(double d)override { coordinate=d; }
    void homeMode(bool h,bool a)override { home=h; armed=a; }
};
struct Fixture {
    Fake io; Axis a{io}; Sample s; uint32_t now=0;
    Fixture() { a.cfg.present=a.cfg.commissioned=1; s.healthy=true; tick(); }
    void tick(bool lease=true,bool emergency=false) { now+=10; a.tick(s,now,lease,emergency); }
    void arrive() { s.position=a.target; s.speed=0; s.ramp=0; tick(); }
    void zero() { CHECK(a.zero()==nullptr); s.position=0; }
};
void maths() {
    Config c; CHECK(validate(c)==nullptr); int32_t n;
    CHECK(counts(360,c,n)&&n==3200); c.gear=50; CHECK(counts(90,c,n)&&n==40000);
    c.fullSteps=400; CHECK(counts(-45,c,n)&&n==-40000);
    CHECK(!counts(std::numeric_limits<double>::quiet_NaN(),c,n)); CHECK(!counts(1e100,c,n));
    CHECK(signed24(0xffffff)==-1); CHECK(signed24(0x800000)==-8388608); CHECK(signed24(0x7fffff)==8388607);
    CHECK(wrap(359)==-1); CHECK(wrap(-359)==1);
    CHECK(crc32("123456789",9)==0xcbf43926);
    for(uint64_t i=0;i<100000;++i) CHECK(grayToBinary(i^(i>>1))==i);
    c=Config{}; c.mtBits=12; c.stBits=13; c.frameBits=26; c.shift=1;
    for(uint64_t i:{0ull,8191ull,8192ull,33554431ull}) {
        uint64_t raw=(i^(i>>1))<<1; CHECK(decode(raw,c)==i); CHECK(singleturn(raw,c)==(i&8191));
    }
    c=Config{}; c.encoderZero=4095; CHECK(std::abs(encoderAngle(0,c)-360.0/4096)<1e-8);
    for(unsigned ma=50;ma<=3000;ma+=10) {
        auto cur=currentCode(ma,.35,.033); CHECK(cur.actual>0&&cur.actual<=ma+1e-9);
        CHECK(cur.global==0||cur.global>=32); CHECK(cur.hold<=cur.run);
    }
    CHECK(std::abs(rms(31,0,.033)-rms(31,256,.033))<1e-9);
    c=Config{}; c.microsteps=3; CHECK(validate(c)!=nullptr);
    c=Config{}; c.speed=std::numeric_limits<double>::infinity(); CHECK(validate(c)!=nullptr);
    c=Config{}; c.gear=0; CHECK(validate(c)!=nullptr);
    c=Config{}; c.low=50; CHECK(validate(c)!=nullptr);
    c=Config{}; c.hold=std::numeric_limits<double>::quiet_NaN(); CHECK(validate(c)!=nullptr);
    c=Config{}; c.sgt=0; CHECK(validate(c)==nullptr); c.sgt=-64; CHECK(validate(c)==nullptr); c.sgt=64; CHECK(validate(c)!=nullptr);
    c=Config{}; c.mtBits=12; CHECK(validate(c)!=nullptr);
    c=Config{}; c.frameBits=255; CHECK(validate(c)!=nullptr);
}
void motion() {
    Fixture f; CHECK(!f.a.enabled); CHECK(f.a.move(10,false,false,f.now)!=nullptr);
    CHECK(f.a.move(3,true,true,f.now)!=nullptr);
    CHECK(f.a.move(2,true,true,f.now)==nullptr&&f.io.on);
    f.arrive(); CHECK(f.a.state==State::Idle&&!f.a.referenced);
    f.zero(); CHECK(f.a.move(45,false,false,f.now)==nullptr);
    f.s.speed=0; f.tick(); CHECK(f.a.state==State::Move);
    CHECK(f.a.zero()!=nullptr); CHECK(f.a.test(-45,45,1,0,f.now)!=nullptr);
    f.arrive(); CHECK(f.a.state==State::Idle);
    CHECK(f.a.move(91,false,false,f.now)!=nullptr); CHECK(f.a.move(-90,false,false,f.now)==nullptr);
    f.s.speed=-5; f.s.position=20; f.tick(false); CHECK(f.a.state==State::Stopping&&f.io.stops>0);
    f.s.speed=0; f.tick(false); CHECK(f.a.state==State::Idle&&f.io.coordinate==20);
    f.a.release(); CHECK(!f.io.on&&!f.a.referenced);
    f.zero(); f.tick(true,true); CHECK(f.a.state==State::Fault&&!f.io.on&&!f.a.referenced);
    CHECK(f.a.move(1,true,true,f.now)!=nullptr); CHECK(f.a.clear(true)!=nullptr);
    CHECK(f.a.clear(false)==nullptr&&!f.a.enabled&&!f.a.referenced);
}
void testsAndLimits() {
    Fixture f; f.zero(); CHECK(f.a.test(-45,45,2,0,f.now)==nullptr);
    CHECK(f.a.target==-45);
    f.arrive(); f.tick(); CHECK(f.a.target==45);
    f.arrive(); f.tick(); CHECK(f.a.target==-45);
    f.arrive(); f.tick(); CHECK(f.a.cycles==1&&f.a.target==45);
    f.arrive(); f.tick(); f.arrive(); f.tick(); CHECK(f.a.cycles==2&&f.a.state==State::Idle);
    Fixture h; h.zero(); h.a.cfg.hallMask=5; h.s.halls=1; h.tick();
    CHECK(h.a.move(-1,true,false,h.now)!=nullptr); CHECK(h.a.move(1,true,false,h.now)==nullptr);
    h.arrive(); CHECK(h.a.state==State::Idle);
    h.s.halls=0; h.tick(); CHECK(h.a.move(45,false,false,h.now)==nullptr);
    h.a.cfg.hardLimits=1; h.s.ramp=0x20; h.s.speed=0; h.tick();
    CHECK(h.a.state==State::Fault);
    Fixture b; b.a.cfg.hallMask=5; b.s.halls=5; b.tick(); CHECK(b.a.state==State::Fault);
    Fixture bus; bus.zero(); bus.a.cfg.hallMask=1; bus.s.hallHealthy=false; bus.tick(); CHECK(bus.a.state==State::Fault);
}
void homing() {
    Fixture f; f.a.cfg.homeMethod=2;
    CHECK(f.a.home(false,f.now)!=nullptr); f.a.cfg.sgTuned=1; f.a.cfg.sgt=0;
    CHECK(f.a.home(false,f.now)==nullptr&&f.io.home);
    f.s.speed=-5; f.now+=100; f.tick(); CHECK(f.a.sgArmed&&f.io.armed);
    f.s.ramp=0x40; f.s.speed=0; f.s.position=-40; f.tick();
    CHECK(f.a.state==State::BackoffMin&&!f.a.sgArmed&&!f.a.referenced);
    f.arrive(); CHECK(f.a.referenced&&f.a.state==State::Idle&&!f.io.home);
    Fixture nohit; nohit.a.cfg.homeMethod=1; nohit.a.cfg.hallMask=1;
    CHECK(nohit.a.home(false,nohit.now)==nullptr); nohit.arrive(); CHECK(nohit.a.state==State::Fault);
    Fixture timeout; timeout.a.cfg.homeMethod=1; timeout.a.cfg.hallMask=1;
    CHECK(timeout.a.home(false,timeout.now)==nullptr); timeout.now+=timeout.a.cfg.homeTimeout+1; timeout.tick(); CHECK(timeout.a.state==State::Fault);
    Fixture stuck; stuck.a.cfg.homeMethod=1; stuck.a.cfg.hallMask=1; stuck.s.halls=1; stuck.tick();
    CHECK(stuck.a.home(false,stuck.now)==nullptr&&stuck.a.state==State::HomeRelease); stuck.arrive(); CHECK(stuck.a.state==State::Fault);
    Fixture abort; abort.a.cfg.homeMethod=1; abort.a.cfg.hallMask=1;
    CHECK(abort.a.home(false,abort.now)==nullptr); abort.a.stop(abort.now); CHECK(!abort.a.enabled&&!abort.a.referenced&&abort.a.state==State::Off);
    Fixture scan; scan.a.cfg.homeMethod=1; scan.a.cfg.hallMask=5; scan.a.cfg.hardLimits=1;
    CHECK(scan.a.home(true,scan.now)==nullptr);
    scan.s.position=-50; scan.s.ramp=0x10; scan.tick(); CHECK(scan.a.state==State::BackoffMin);
    scan.arrive(); CHECK(scan.a.state==State::SeekMax);
    scan.s.position=200; scan.s.ramp=0x20; scan.tick(); CHECK(scan.a.state==State::BackoffMax&&scan.a.measured);
    CHECK(scan.a.measuredLow==-100&&scan.a.measuredHigh==100);
    scan.arrive(); CHECK(scan.a.state==State::Center); scan.arrive(); CHECK(scan.a.referenced&&scan.a.state==State::Idle);
}
void feedbackAndIsolation() {
    Fixture f,g; f.zero(); g.zero();
    CHECK(f.a.move(45,false,false,f.now)==nullptr); CHECK(g.a.move(-45,false,false,g.now)==nullptr);
    f.s.healthy=false; f.tick(); CHECK(f.a.state==State::Fault&&g.a.state==State::Move&&g.io.on);
    Fixture e; e.zero(); e.a.cfg.encoder=e.a.cfg.frameConfirmed=e.a.cfg.zeroValid=1;
    e.s.encoderValid=true; e.s.encoderDegrees=10; e.tick(); CHECK(e.a.state!=State::Fault);
    e.now+=260; e.tick(); CHECK(e.a.state==State::Fault);
    Fixture valid; valid.a.cfg.encoder=valid.a.cfg.frameConfirmed=valid.a.cfg.zeroValid=1;
    valid.s.encoderValid=true; valid.s.encoderDegrees=-45; valid.tick(); CHECK(valid.a.useEncoder()==nullptr&&valid.a.target==-45);
    Fixture roll; roll.zero(); roll.now=0xfffffff0; CHECK(roll.a.move(10,false,false,roll.now)==nullptr);
    roll.now=10; roll.tick(); CHECK(roll.a.state==State::Move);
}
int main() { maths(); motion(); testsAndLimits(); homing(); feedbackAndIsolation(); std::cout<<checks<<" assertions passed\n"; }
