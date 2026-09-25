#include "hardware.h"
#include <cassert>
#include <iostream>
using namespace bench;
int main(){
 for(auto& p:pins)p=HIGH;
 Config c;c.present=c.commissioned=1; Driver d(0,c);
 SPI.regs[0][4]=0x30000000;
 assert(d.configure());assert(SPI.regs[0][0]==0);assert((SPI.regs[0][0x6c]&0xc0000000)==0);
 assert(SPI.regs[0][0x20]==3&&pins[21]==HIGH);
 auto cur=currentCode(c.current,c.hold,c.rsense);assert(SPI.regs[0][0xb]==cur.global);
 assert(((SPI.regs[0][0x10]>>8)&31)==cur.run);
 assert(d.enable()&&pins[21]==LOW);
 d.start(-45,c.speed);assert(int32_t(SPI.regs[0][0x2d])==-400);assert(SPI.regs[0][0x20]==0);
 assert(SPI.regs[0][0x28]==acceleration(c.decel,c));
 SPI.regs[0][0x22]=100;d.disable();assert(pins[21]==HIGH&&SPI.regs[0][0x20]==1);
 assert(!d.enable());
 SPI.regs[0][0x22]=0;SPI.regs[0][0x21]=17;assert(d.enable());assert(SPI.regs[0][0x2d]==17);
 d.homeMode(true,true);assert(SPI.regs[0][0x34]==(1u<<10));assert(!(SPI.regs[0][0x34]&(1u<<11)));
 c.hardLimits=1;c.hallMask=5;d.homeMode(false,false);assert(SPI.regs[0][0x34]==15);
 SPI.regs[0][0x22]=0xffffff;auto sample=d.sample();assert(sample.speed<0);
 SPI.regs[0][1]=1;assert(!d.sample().healthy);SPI.regs[0][1]=0;
 SPI.regs[0][4]|=64;assert(!d.probe());SPI.regs[0][4]&=~64u;
 SPI.regs[0][0x22]=200;d.start(1,1);assert(!d.configured&&pins[21]==HIGH);
 HallBus halls;halls.begin();uint8_t active;Wire.regs[0x12]=0b11110110;
 assert(halls.read(active)&&active==9);Wire.available=false;assert(!halls.read(active));
 Ssi ssi;ssi.begin();c=Config{};c.encoder=c.frameConfirmed=1;Sample s;
 serialBits={1};for(unsigned i=0;i<12;++i)serialBits.push_back(0);serialBits.push_back(0);
 ssi.read(0,c,s);assert(s.encoderValid&&s.encoderCount==0);
 serialBits.assign(14,1);ssi.read(0,c,s);assert(!s.encoderValid);
 serialBits.assign(14,0);ssi.read(0,c,s);assert(!s.encoderValid);
 c.frameConfirmed=0;serialBits={1};for(unsigned i=0;i<13;++i)serialBits.push_back(0);
 ssi.read(0,c,s);assert(!s.encoderValid);
 std::cout<<"hardware fake: SPI register order, stopped-only enable, signed speed, Hall failure and SSI line checks passed\n";
}
