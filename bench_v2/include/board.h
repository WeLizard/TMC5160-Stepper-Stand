#pragma once
// ESP32-WROOM / DOIT DevKit V1. GPIO16/17 conflict with PSRAM on WROVER.
namespace board {
constexpr int SCK=18, MISO=19, MOSI=23;
constexpr int CS[2]={5,17}, EN[2]={21,16};
constexpr int SDA=27, SCL=22; // Legacy STEP/DIR unused in SPI motion mode.
constexpr int SSI_CLK[2]={14,13}, SSI_DATA[2]={34,35};
constexpr int ESTOP=39; // External 10k pull-up; NC contact to GND. No internal pull-up!
constexpr int SOL_A=25, SOL_B=26, SOL_EN=15, SOL_HALL[2]={32,33};
constexpr uint8_t MCP_ADDR=0x20;
inline void safeOutputs() {
    for(int i=0;i<2;++i) { digitalWrite(EN[i],HIGH); pinMode(EN[i],OUTPUT); digitalWrite(CS[i],HIGH); pinMode(CS[i],OUTPUT); }
    digitalWrite(SOL_EN,LOW); pinMode(SOL_EN,OUTPUT);
    digitalWrite(SOL_A,LOW); digitalWrite(SOL_B,LOW); pinMode(SOL_A,OUTPUT); pinMode(SOL_B,OUTPUT);
    pinMode(ESTOP,INPUT);
}
}
