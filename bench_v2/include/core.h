#pragma once
// No Arduino dependencies. Positions/speeds refer to the OUTPUT shaft.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace bench {
constexpr double kClock = 12000000.0;
constexpr uint32_t kLeaseMs = 1500;
struct Config {
    double gear=1, rsense=.033, hold=.35, speed=10, accel=20, decel=20;
    double low=-90, high=90, homeCoordinate=-95, homeSpeed=5, travel=220;
    double backoff=3, sgMinSpeed=3, encoderTolerance=2;
    uint32_t homeTimeout=120000, encoderZero=0;
    uint16_t fullSteps=200, microsteps=16, current=300, homeCurrent=200;
    uint8_t present=0, commissioned=0, hallMask=0, hardLimits=0;
    uint8_t homeMethod=0, homeHall=0, sgTuned=0, reverse=0;
    uint8_t encoder=0, frameConfirmed=0, zeroValid=0, encoderReverse=0;
    uint8_t frameBits=12, stBits=12, mtBits=0, shift=0, grayCode=1;
    // SSI frame profile is UNCONFIRMED by default, not inferred from part number.
    int8_t sgt=0; // A valid SGT value; NOT a disable switch.
};
inline bool range(double n,double lo,double hi) { return std::isfinite(n)&&n>=lo&&n<=hi; }
inline double scale(const Config& c) { return c.fullSteps*double(c.microsteps)*c.gear/360; }
inline bool counts(double d,const Config& c,int32_t& result) {
    double n=d*scale(c); if(!range(n,-2e9,2e9)) return false;
    result=int32_t(std::llround(n)); return true;
}
inline int32_t signed24(uint32_t n) { n&=0xffffff; return (n&0x800000)?int32_t(n)-0x1000000:int32_t(n); }
inline uint64_t mask(unsigned bits) { return (uint64_t(1)<<bits)-1; }
inline uint64_t grayToBinary(uint64_t n) { for(unsigned s=32;s;s>>=1) n^=n>>s; return n; }
inline uint64_t decode(uint64_t frame,const Config& c) {
    uint64_t word=(frame>>c.shift)&mask(c.stBits+c.mtBits);
    return c.grayCode?grayToBinary(word):word; // Convert complete position, not ST slice of Gray.
}
inline uint32_t singleturn(uint64_t frame,const Config& c) { return uint32_t(decode(frame,c)&mask(c.stBits)); }
inline double wrap(double d) { d=std::fmod(d+180,360); return (d<0?d+360:d)-180; }
inline double encoderAngle(uint32_t n,const Config& c) {
    double d=wrap((double(n)-c.encoderZero)*360/double(uint64_t(1)<<c.stBits));
    return c.encoderReverse?-d:d;
}
inline uint32_t velocity(double d,const Config& c) { return uint32_t(std::llround(d*scale(c)*16777216/kClock)); }
inline uint32_t acceleration(double d,const Config& c) {
    return std::max(1u,uint32_t(std::llround(d*scale(c)*2199023255552/(kClock*kClock))));
}
inline const char* validate(const Config& c) {
    if(c.present>1||c.commissioned>1||c.hardLimits>1||c.reverse>1||c.encoder>1||c.frameConfirmed>1||c.zeroValid>1||c.encoderReverse>1||c.grayCode>1||c.sgTuned>1) return "INVALID_FLAG";
    if(c.fullSteps<20||c.fullSteps>2000||!c.microsteps||c.microsteps>256||(c.microsteps&(c.microsteps-1))) return "INVALID_STEPS";
    if(!range(c.gear,.01,1000)||!range(c.rsense,.01,.2)||!range(c.hold,.1,1)) return "INVALID_SCALE";
    if(c.current<50||c.current>3000||c.homeCurrent<50||c.homeCurrent>c.current||c.homeCurrent<.325/c.rsense/std::sqrt(2.0)*1000/256) return "INVALID_CURRENT";
    if(!range(c.low,-170,0)||!range(c.high,0,170)||c.low>=c.high) return "INVALID_LIMITS";
    if(!range(c.speed,.1,60)||!range(c.accel,.1,600)||!range(c.decel,.1,600)) return "INVALID_RAMP";
    if(!range(c.homeSpeed,.1,20)||!range(c.sgMinSpeed,.1,c.homeSpeed)||!range(c.travel,1,350)||!range(c.backoff,.1,10)||c.backoff>=c.travel||!range(c.homeCoordinate,-175,175)) return "INVALID_HOME_GEOMETRY";
    if(c.homeTimeout<1000||c.homeTimeout>180000||c.homeMethod>2||c.homeHall>1||c.hallMask>7||c.sgt<-64||c.sgt>63) return "INVALID_HOME_OPTIONS";
    if(c.stBits<12||c.stBits>22||c.mtBits>12||c.frameBits<c.stBits+c.mtBits+c.shift||c.frameBits>40||c.encoderZero>mask(c.stBits)||!range(c.encoderTolerance,.2,20)) return "INVALID_SSI_PROFILE";
    int32_t ignored;
    double v=std::max(c.speed,c.homeSpeed)*scale(c)*16777216/kClock;
    double a=std::max(c.accel,c.decel)*scale(c)*2199023255552/(kClock*kClock);
    if(!counts(720,c,ignored)||v<10||v>8388095||a>65535) return "REGISTER_RANGE";
    return nullptr;
}
struct Current { uint8_t run=0, hold=0, global=0; double actual=0; };
inline double rms(unsigned cs,unsigned gs,double r) { return (gs?gs:256)/256.0*(cs+1)/32.0*.325/r/std::sqrt(2.0)*1000; }
inline Current currentCode(double requested,double hold,double r) {
    Current best;
    for(unsigned cs=0;cs<32;++cs) for(unsigned gs=32;gs<=256;++gs) {
        double ma=rms(cs,gs,r);
        if(ma<=requested+1e-9&&ma>=best.actual-1e-9) { best.run=cs; best.global=gs==256?0:gs; best.actual=ma; }
    }
    best.hold=uint8_t(std::max(0,int(std::floor((best.run+1)*hold))-1)); return best;
}
inline uint32_t crc32(const void* data,size_t len) {
    uint32_t crc=0xffffffff; auto p=static_cast<const uint8_t*>(data);
    while(len--) { crc^=*p++; for(unsigned i=0;i<8;++i) crc=(crc>>1)^(0xedb88320u&(0u-(crc&1))); } return ~crc;
}
enum class State:uint8_t { Off,Idle,Move,Test,Dwell,HomeRelease,SeekMin,SeekMax,BackoffMin,BackoffMax,Center,Stopping,Fault };
inline const char* name(State s) {
    static const char* n[]={"OFF","IDLE","MOVE","TEST","DWELL","HOME_RELEASE","HOME_MIN","HOME_MAX","BACKOFF_MIN","BACKOFF_MAX","CENTER","STOPPING","FAULT"}; return n[unsigned(s)];
}
struct Sample {
    double position=0,speed=0,encoderDegrees=0;
    uint64_t encoderRaw=0;
    uint32_t driver=0,ramp=0,encoderCount=0;
    uint16_t sg=0; uint8_t halls=0;
    bool healthy=false,hallHealthy=true,encoderValid=false;
};
struct IO {
    virtual ~IO()=default;
    virtual bool enable()=0;
    virtual void disable()=0;
    virtual void start(double target,double speed)=0;
    virtual void stop()=0;
    virtual void position(double coordinate)=0; // Precondition: observed VACTUAL == 0.
    virtual void homeMode(bool active,bool sgArmed)=0;
};
class Axis {
public:
    Config cfg; Sample sample; State state=State::Off;
    bool enabled=false,referenced=false,sgArmed=false,measured=false;
    double target=0,measuredLow=0,measuredHigh=0;
    uint32_t cycles=0; const char* fault="";
    explicit Axis(IO& driver):io(driver) {}
    bool busy()const { return state!=State::Off&&state!=State::Idle&&state!=State::Fault; }
    bool homing()const { return state>=State::HomeRelease&&state<=State::Center; }
    bool rest()const { return sample.speed==0; }
    bool reached()const { return rest()&&std::abs(target-sample.position)<=.6/scale(cfg); }
    const char* ready()const {
        if(state==State::Fault) return "FAULT_LATCHED";
        if(!cfg.present||!cfg.commissioned) return "COMMISSION_FIRST";
        if(!sample.healthy) return "DRIVER_NOT_READY";
        if(cfg.hallMask&&!sample.hallHealthy) return "HALL_BUS_FAILURE";
        return validate(cfg);
    }
    void trip(const char* why) { io.disable(); enabled=referenced=sgArmed=false; state=State::Fault; fault=why; encoderBad=false; }
    void release() { io.disable(); enabled=referenced=sgArmed=false; if(state!=State::Fault) state=State::Off; }
    const char* clear(bool emergency) {
        if(emergency) return "ESTOP_ACTIVE";
        if(cfg.present&&!sample.healthy) return "DRIVER_NOT_READY";
        release(); fault=""; state=State::Off; return nullptr;
    }
    const char* hold() {
        if(auto e=ready()) return e;
        if(busy()) return "AXIS_BUSY";
        if(!enabled&&!io.enable()) return "ENABLE_FAILED";
        enabled=true; state=State::Idle; return nullptr;
    }
    const char* zero() {
        if(busy()||!rest()) return "STOP_BEFORE_ZERO";
        if(auto e=hold()) return e;
        io.position(0); sample.position=target=0; referenced=true; return nullptr;
    }
    const char* useEncoder() {
        if(!cfg.encoder||!cfg.frameConfirmed||!cfg.zeroValid||!sample.encoderValid) return "ENCODER_NOT_CALIBRATED";
        if(busy()||!rest()) return "STOP_BEFORE_ZERO";
        if(!inside(sample.encoderDegrees)) return "ENCODER_OUTSIDE_LIMITS";
        if(auto e=hold()) return e;
        io.position(sample.encoderDegrees); sample.position=target=sample.encoderDegrees; referenced=true; return nullptr;
    }
    const char* move(double value,bool relative,bool jog,uint32_t now) {
        if(auto e=ready()) return e;
        if(busy()) return "AXIS_BUSY";
        if(!std::isfinite(value)) return "INVALID_TARGET";
        if(!referenced&&!(jog&&relative&&std::abs(value)<=2)) return "REFERENCE_REQUIRED";
        double destination=relative?sample.position+value:value; int32_t n;
        if(!counts(destination,cfg,n)) return "POSITION_OVERFLOW";
        if(referenced&&!inside(destination)) return "SOFT_LIMIT";
        if(blocked(destination-sample.position)) return "HALL_LIMIT_ACTIVE";
        if(!enabled&&!io.enable()) return "ENABLE_FAILED";
        enabled=true; go(destination,referenced?cfg.speed:std::min(cfg.speed,2.0),State::Move,now); return nullptr;
    }
    const char* test(double low,double high,uint32_t count,uint32_t dwell,uint32_t now) {
        if(auto e=ready()) return e;
        if(busy()) return "AXIS_BUSY";
        if(!referenced) return "REFERENCE_REQUIRED";
        if(!inside(low)||!inside(high)||low>=high||!count||count>10000||dwell>30000) return "INVALID_TEST";
        if(blocked(low-sample.position)) return "HALL_LIMIT_ACTIVE";
        testLow=low; testHigh=high; total=count; dwellMs=dwell; phase=0; cycles=0;
        go(low,cfg.speed,State::Test,now); return nullptr;
    }
    const char* home(bool scan,uint32_t now) {
        if(auto e=ready()) return e;
        if(busy()) return "AXIS_BUSY";
        if(!cfg.homeMethod) return "SELECT_HOME_METHOD";
        if(cfg.homeMethod==1&&!(cfg.hallMask&(1<<cfg.homeHall))) return "HOME_HALL_REQUIRED";
        if(cfg.homeMethod==2&&!cfg.sgTuned) return "TUNE_STALLGUARD_FIRST";
        if(scan&&cfg.homeMethod==1&&(cfg.homeHall!=0||(cfg.hallMask&5)!=5)) return "MIN_MAX_HALLS_REQUIRED";
        if(scan&&cfg.encoder&&cfg.zeroValid) return "CLEAR_ENCODER_ZERO_BEFORE_SCAN";
        if(cfg.homeMethod==2&&(sample.halls&cfg.hallMask&5)) return "MOVE_CLEAR_OF_LIMITS_FIRST";
        if(!enabled&&!io.enable()) return "ENABLE_FAILED";
        enabled=true; referenced=sgArmed=false; scanning=scan; measured=false; homeStart=now;
        io.homeMode(true,false);
        if(cfg.homeMethod==1&&hit(false)) {
            if(blocked(1)) { release(); return "OPPOSITE_LIMIT_ACTIVE"; }
            go(sample.position+cfg.backoff,cfg.homeSpeed,State::HomeRelease,now);
        } else seek(false,now);
        return nullptr;
    }
    void stop(uint32_t now) {
        if(state==State::Fault) return;
        if(homing()) { release(); return; }
        if(!enabled) { state=State::Off; return; }
        io.stop(); state=State::Stopping; stage=now;
    }
    void tick(const Sample& s,uint32_t now,bool lease,bool emergency) {
        sample=s;
        if(emergency) { if(state!=State::Fault) trip("EMERGENCY_STOP"); return; }
        if(state==State::Fault) return;
        if(enabled&&!sample.healthy) { trip("DRIVER_FAULT_OR_SPI_LOSS"); return; }
        if(enabled&&cfg.hallMask&&!sample.hallHealthy) { trip("HALL_BUS_FAILURE"); return; }
        if((cfg.hallMask&sample.halls&5)==5) { trip("MIN_AND_MAX_ACTIVE"); return; }
        if(busy()&&!lease&&state!=State::Stopping) { stop(now); return; }
        if(enabled&&referenced&&cfg.encoder&&cfg.frameConfirmed&&cfg.zeroValid) {
            bool bad=!sample.encoderValid||std::abs(wrap(sample.encoderDegrees-sample.position))>cfg.encoderTolerance;
            if(!bad) encoderBad=false;
            else if(!encoderBad) { encoderBad=true; encoderBadAt=now; }
            else if(uint32_t(now-encoderBadAt)>=250) { trip("ENCODER_LOSS_OR_FOLLOWING_ERROR"); return; }
        }
        if(homing()&&uint32_t(now-homeStart)>cfg.homeTimeout) { trip("HOME_TIMEOUT"); return; }
        if(busy()&&uint32_t(now-stage)>180000) { trip("MOVE_TIMEOUT"); return; }
        if(!homing()&&busy()&&(hardwareBlocked()||blocked(sample.speed))) { trip("HALL_LIMIT"); return; }
        if(homing()) {
            bool leftExpected=state==State::SeekMin&&cfg.homeMethod==1&&cfg.homeHall==0;
            bool rightExpected=state==State::SeekMax&&cfg.homeMethod==1;
            if((!leftExpected&&direction<0&&leftStop())||(!rightExpected&&direction>0&&rightStop())) { trip("UNEXPECTED_LIMIT"); return; }
        }
        switch(state) {
        case State::Move: if(reached()) state=State::Idle; break;
        case State::Stopping:
            if(rest()) { target=sample.position; io.position(target); state=State::Idle; } break;
        case State::Test: if(reached()) { state=State::Dwell; stage=now; } break;
        case State::Dwell:
            if(uint32_t(now-stage)<dwellMs) break;
            if(phase==0) { phase=1; go(testHigh,cfg.speed,State::Test,now); }
            else if(phase==1) { phase=2; go(testLow,cfg.speed,State::Test,now); }
            else if(++cycles>=total) state=State::Idle;
            else { phase=1; go(testHigh,cfg.speed,State::Test,now); } break;
        case State::HomeRelease:
            if(reached()) { if(hit(false)) trip("HOME_HALL_STUCK"); else seek(false,now); } break;
        case State::SeekMin: case State::SeekMax: {
            bool positive=state==State::SeekMax;
            if(cfg.homeMethod==2&&!sgArmed&&uint32_t(now-stage)>=100&&std::abs(sample.speed)>=cfg.sgMinSpeed) {
                sgArmed=true; io.homeMode(true,true);
            }
            if(cfg.homeMethod==2&&!sgArmed&&uint32_t(now-stage)>2000) { trip("SG_SPEED_NOT_REACHED"); break; }
            if(hit(positive)&&!homeLatched) { homeLatched=true; hitPosition=sample.position; io.stop(); }
            if(homeLatched) {
                if(!rest()) break;
                io.position(sample.position); // Neutralize target BEFORE releasing hardware stop.
                io.homeMode(true,false); sgArmed=false;
                if(!positive) {
                    double origin=(scanning?0:cfg.homeCoordinate)+(sample.position-hitPosition);
                    io.position(origin); sample.position=origin;
                    double dest=scanning?cfg.backoff:std::max(cfg.low,cfg.homeCoordinate+cfg.backoff);
                    if(dest>cfg.high) { trip("HOME_OUTSIDE_LIMITS"); break; }
                    go(dest,cfg.homeSpeed,State::BackoffMin,now);
                } else {
                    double half=hitPosition/2;
                    measuredLow=-half; measuredHigh=half; measured=half>0;
                    if(!measured||cfg.low<measuredLow+cfg.backoff||cfg.high>measuredHigh-cfg.backoff) { trip("LIMITS_OUTSIDE_MEASURED_RANGE"); break; }
                    double centered=sample.position-half; io.position(centered); sample.position=centered;
                    go(half-cfg.backoff,cfg.homeSpeed,State::BackoffMax,now);
                }
            } else if(reached()) trip("HOME_TRAVEL_EXHAUSTED");
            break; }
        case State::BackoffMin:
            if(reached()) {
                if(cfg.homeMethod==1&&hall(cfg.homeHall)) { trip("HOME_HALL_NOT_RELEASED"); break; }
                if(scanning) seek(true,now);
                else { io.homeMode(false,false); referenced=true; state=State::Idle; }
            } break;
        case State::BackoffMax:
            if(reached()) {
                if(cfg.homeMethod==1&&hall(2)) { trip("MAX_HALL_NOT_RELEASED"); break; }
                io.homeMode(false,false); go(0,cfg.speed,State::Center,now);
            } break;
        case State::Center: if(reached()) { referenced=true; state=State::Idle; } break;
        default: break;
        }
    }
private:
    IO& io;
    uint32_t stage=0,homeStart=0,encoderBadAt=0,total=0,dwellMs=0;
    double testLow=0,testHigh=0,hitPosition=0;
    unsigned phase=0; int direction=0;
    bool scanning=false,encoderBad=false,homeLatched=false;
    bool inside(double n)const { return range(n,cfg.low,cfg.high); }
    bool hall(unsigned n)const { return (cfg.hallMask&sample.halls&(1u<<n))!=0; }
    bool leftStop()const { return hall(0)||(cfg.hardLimits&&(cfg.hallMask&1)&&(sample.ramp&0x10)); }
    bool rightStop()const { return hall(2)||(cfg.hardLimits&&(cfg.hallMask&4)&&(sample.ramp&0x20)); }
    bool blocked(double d)const { return (d<0&&hall(0))||(d>0&&hall(2)); }
    bool hardwareBlocked()const { return (direction<0&&leftStop())||(direction>0&&rightStop()); }
    bool hit(bool right)const {
        if(cfg.homeMethod==2) return sgArmed&&(sample.ramp&0x40);
        return right?rightStop():(cfg.homeHall==0?leftStop():hall(1));
    }
    void go(double d,double speed,State next,uint32_t now) {
        direction=(d>sample.position)-(d<sample.position);
        target=d; stage=now; state=next; io.start(d,speed);
    }
    void seek(bool positive,uint32_t now) {
        sgArmed=false; homeLatched=false; io.homeMode(true,false);
        go(sample.position+(positive?cfg.travel:-cfg.travel),cfg.homeSpeed,positive?State::SeekMax:State::SeekMin,now);
    }
};
} // namespace bench
