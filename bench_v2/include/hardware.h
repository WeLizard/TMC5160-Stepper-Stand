#pragma once
#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include "board.h"
#include "core.h"

namespace bench {
// All calls belong to the control task. No bus access in HTTP handlers/interrupts.
class Driver final:public IO {
public:
    Driver(unsigned channel,Config& config):id(channel),cfg(config) {}
    bool configured=false;
    void write(uint8_t reg,uint32_t value) { transfer(reg|0x80,value); }
    uint32_t read(uint8_t reg) { transfer(reg,0); return transfer(reg,0); }
    bool probe() {
        uint32_t value=read(0x04); // IOIN: version + physically selected SD_MODE.
        return (value>>24)==0x30 && !(value&(1u<<6));
    }
    bool configure() {
        digitalWrite(board::EN[id],HIGH); configured=false;
        if(!cfg.present||validate(cfg)||!probe()) return false;
        if(signed24(read(0x22))!=0) { write(0x27,0); write(0x20,1); return false; }
        write(0x27,0); write(0x20,3); // VMAX=0; hold ramp BEFORE clear/reset.
        write(0x00,cfg.reverse?1u<<4:0); // SpreadCycle, no stop_enable, no stealthChop.
        write(0x0a,(2u<<18)|(4u<<8)); // Same gate strength/BBM defaults as legacy tommag.
        write(0x01,7); // GSTAT W1C, only at explicit reinitialization.
        write(0x11,10); // TPOWERDOWN ~218 ms at nominal 12MHz.
        unsigned mres=0; for(unsigned n=cfg.microsteps;n<256;n*=2) ++mres;
        chop=(mres<<24)|(2u<<15)|(4u<<4)|5u; // TOFF5, TBL2, HSTRT4; protections remain ON.
        write(0x6c,chop);
        runCode=currentCode(cfg.current,cfg.hold,cfg.rsense);
        homeCode=currentCode(cfg.homeCurrent,cfg.hold,cfg.rsense);
        applyCurrent(runCode);
        write(0x13,0); write(0x15,0); write(0x33,0); // TPWMTHRS, THIGH, VDCMIN off.
        write(0x6d,(uint32_t(uint8_t(cfg.sgt)&0x7f)<<16)); // SEMIN0 disables coolStep.
        // TSTEP uses time between 1/256 full steps, independent of CHOPCONF.MRES.
        double threshold=kClock/(cfg.sgMinSpeed*cfg.fullSteps*cfg.gear/360*256);
        write(0x14,uint32_t(std::max(1.0,std::min(1048575.0,threshold))));
        homeMode(false,false);
        position(0); ramp(cfg.speed);
        configured=read(0x00)==(cfg.reverse?1u<<4:0) && read(0x6c)==chop && probe();
        return configured;
    }
    bool enable()override {
        if(!configured||!probe()||signed24(read(0x22))!=0) return false;
        position(int32_t(read(0x21))/scale(cfg));
        digitalWrite(board::EN[id],LOW); return true;
    }
    void disable()override {
        // EN first, even if SPI is broken. Any lost coordinate is invalidated by Axis.
        digitalWrite(board::EN[id],HIGH);
        if(configured) { write(0x27,0); write(0x20,1); } // Velocity mode ramps to zero with coils OFF.
        // HOLD mode would retain a nonzero VACTUAL; never use it for a moving release!
    }
    void start(double target,double speed)override {
        if(signed24(read(0x22))!=0) { disable(); configured=false; return; }
        int32_t n; if(!counts(target,cfg,n)) { disable(); configured=false; return; }
        // Caller only starts a move when stopped; clear old reference events in HOLD.
        write(0x27,0); write(0x20,3);
        write(0x2d,read(0x21));
        write(0x35,0x10cc); // Clear only W1C fields, not read-only ref-stop bits.
        ramp(speed); write(0x2d,uint32_t(n)); write(0x20,0);
    }
    void stop()override { write(0x27,0); } // Position mode decelerates using DMAX / D1.
    void position(double coordinate)override {
        if(signed24(read(0x22))!=0) { disable(); configured=false; return; }
        int32_t n; if(!counts(coordinate,cfg,n)) { disable(); configured=false; return; }
        write(0x27,0); write(0x20,3); write(0x21,uint32_t(n)); write(0x2d,uint32_t(n));
        write(0x35,0x10cc);
    }
    void homeMode(bool active,bool armed)override {
        applyCurrent(active?homeCode:runCode);
        uint32_t sw=0;
        if(cfg.hardLimits&&(cfg.hallMask&1)) sw|=(1u<<0)|(1u<<2);
        if(cfg.hardLimits&&(cfg.hallMask&4)) sw|=(1u<<1)|(1u<<3);
        if(armed) sw|=1u<<10; // Internal sg_stop: DIAG interrupt not required.
        write(0x34,sw); // en_softstop MUST stay zero with StallGuard2.
    }
    Sample sample() {
        Sample s;
        if(!cfg.present) return s;
        const uint32_t input=read(0x04),gstat=read(0x01);
        s.driver=read(0x6f); s.ramp=read(0x35); s.sg=s.driver&0x3ff;
        const uint32_t failures=(1u<<12)|(1u<<13)|(1u<<25)|(1u<<26)|(1u<<27)|(1u<<28);
        s.healthy=configured&&(input>>24)==0x30&&!(input&(1u<<6))&&!(gstat&7)&&!(s.driver&failures);
        s.position=int32_t(read(0x21))/scale(cfg);
        s.speed=signed24(read(0x22))*kClock/16777216/scale(cfg);
        return s;
    }
    double actualCurrent()const { return runCode.actual; } // Calculated, not a measurement.
private:
    unsigned id; Config& cfg; uint32_t chop=0; Current runCode,homeCode;
    uint32_t transfer(uint8_t address,uint32_t data) {
        SPI.beginTransaction(SPISettings(100000,MSBFIRST,SPI_MODE3));
        digitalWrite(board::CS[id],LOW); SPI.transfer(address);
        uint32_t result=0;
        for(int b=24;b>=0;b-=8) result=(result<<8)|SPI.transfer(uint8_t(data>>b));
        digitalWrite(board::CS[id],HIGH); SPI.endTransaction(); delayMicroseconds(2);
        return result;
    }
    void applyCurrent(Current c) { write(0x0b,c.global); write(0x10,(7u<<16)|(uint32_t(c.run)<<8)|c.hold); }
    void ramp(double speed) {
        write(0x23,0); write(0x24,acceleration(cfg.accel,cfg)); write(0x25,0);
        write(0x26,acceleration(cfg.accel,cfg)); write(0x28,acceleration(cfg.decel,cfg));
        write(0x2a,acceleration(cfg.decel,cfg)); write(0x2b,10); write(0x2c,0);
        write(0x27,velocity(speed,cfg));
    }
};
class HallBus {
public:
    void begin() {
        Wire.begin(board::SDA,board::SCL); Wire.setClock(100000); Wire.setTimeOut(5);
        put(0x0a,0); put(0x00,255); put(0x01,255); put(0x0c,255); put(0x0d,255);
    }
    bool read(uint8_t& active) {
        // IODIR and pull-up read-back catches a reset/misconfigured expander.
        int dir=get(0x00),pull=get(0x0c),raw=get(0x12);
        if(dir!=255||pull!=255||raw<0) return false;
        active=uint8_t(~raw)&0x3f; return true;
    }
private:
    void put(uint8_t reg,uint8_t value) { Wire.beginTransmission(board::MCP_ADDR); Wire.write(reg); Wire.write(value); Wire.endTransmission(); }
    int get(uint8_t reg) {
        Wire.beginTransmission(board::MCP_ADDR); Wire.write(reg);
        if(Wire.endTransmission(false)!=0||Wire.requestFrom(uint8_t(board::MCP_ADDR),uint8_t(1))!=1) return -1;
        return Wire.read();
    }
};
class Ssi {
public:
    void begin() { for(unsigned i=0;i<2;++i) { digitalWrite(board::SSI_CLK[i],HIGH); pinMode(board::SSI_CLK[i],OUTPUT); pinMode(board::SSI_DATA[i],INPUT); } }
    void read(unsigned id,const Config& cfg,Sample& s) {
        if(!cfg.encoder) return;
        // Idle HIGH, 100kHz nominal; >=500us frame gap. Verify on exact AC36 matchcode.
        delayMicroseconds(500); bool idle=digitalRead(board::SSI_DATA[id]);
        uint64_t raw=0; bool tail;
        portENTER_CRITICAL(&mux);
        for(unsigned b=0;b<cfg.frameBits;++b) {
            digitalWrite(board::SSI_CLK[id],LOW); delayMicroseconds(5);
            digitalWrite(board::SSI_CLK[id],HIGH); delayMicroseconds(5);
            raw=(raw<<1)|(digitalRead(board::SSI_DATA[id])?1u:0u);
        }
        digitalWrite(board::SSI_CLK[id],LOW); delayMicroseconds(5);
        digitalWrite(board::SSI_CLK[id],HIGH); delayMicroseconds(5);
        tail=digitalRead(board::SSI_DATA[id]);
        portEXIT_CRITICAL(&mux);
        s.encoderRaw=raw; s.encoderCount=singleturn(raw,cfg);
        s.encoderDegrees=encoderAngle(s.encoderCount,cfg);
        // Standard SSI zero trailing bit. Wrong frame length is not "healthy".
        // Valid zero position is allowed; constant raw==0 is not treated as disconnect.
        s.encoderValid=cfg.frameConfirmed&&idle&&!tail;
    }
private:
    portMUX_TYPE mux=portMUX_INITIALIZER_UNLOCKED;
};
} // namespace bench
