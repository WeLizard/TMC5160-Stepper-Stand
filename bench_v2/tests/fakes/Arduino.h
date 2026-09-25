#pragma once
#include <cstdint>
#include <vector>
constexpr int LOW=0,HIGH=1,OUTPUT=1,INPUT=0,INPUT_PULLUP=2;
inline int pins[64];
inline std::vector<int> serialBits;
inline void digitalWrite(int pin,int value){pins[pin]=value;}
inline void pinMode(int,int){}
inline int digitalRead(int pin){if(pin==34&&!serialBits.empty()){int n=serialBits.front();serialBits.erase(serialBits.begin());return n;}return pins[pin];}
inline void delayMicroseconds(unsigned){}
using portMUX_TYPE=int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) (void)(x)
#define portEXIT_CRITICAL(x) (void)(x)
