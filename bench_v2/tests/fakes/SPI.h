#pragma once
#include "Arduino.h"
constexpr int MSBFIRST=1,SPI_MODE3=3;
struct SPISettings{SPISettings(int,int,int){}};
struct FakeSPI {
 uint32_t regs[2][128]={}; uint8_t pending[2]={}; unsigned offset=0,chip=0; uint8_t address=0; uint32_t input=0,output=0;
 void begin(int=0,int=0,int=0){}
 void beginTransaction(SPISettings){offset=0;input=0;}
 uint8_t transfer(uint8_t b){
  if(offset++==0){chip=pins[5]==LOW?0:1;address=b;output=regs[chip][pending[chip]];return 0;}
  unsigned shift=32-8*(offset-1);input=(input<<8)|b;return uint8_t(output>>shift);
 }
 void endTransaction(){
  unsigned r=address&0x7f;
  if(address&0x80){
   if(r==1||r==0x35)regs[chip][r]&=~input;else regs[chip][r]=input;
   if(r==0x20&&input==3)regs[chip][0x35]&=~0x30u;
  } else pending[chip]=r;
 }
} inline SPI;
