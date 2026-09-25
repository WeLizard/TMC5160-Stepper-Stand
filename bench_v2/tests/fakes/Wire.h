#pragma once
#include <cstdint>
struct FakeWire {
 bool available=true; uint8_t regs[256]={},reg=0; unsigned offset=0;
 void begin(int,int){} void setClock(int){} void setTimeOut(int){}
 void beginTransmission(unsigned){offset=0;}
 void write(uint8_t v){if(offset++==0)reg=v;else regs[reg]=v;}
 int endTransmission(bool=true){return available?0:1;}
 int requestFrom(uint8_t,uint8_t){return available?1:0;}
 int read(){return regs[reg];}
} inline Wire;
