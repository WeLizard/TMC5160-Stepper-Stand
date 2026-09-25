#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <atomic>
#include <limits>
#include <type_traits>
#include "hardware.h"
#include "fields.h"
using namespace bench;

Config initial[2];
// Driver references are rebound indirectly by copying Config into initial before configure.
Driver drivers[2]={{0,initial[0]},{1,initial[1]}};
Axis axes[2]={Axis(drivers[0]),Axis(drivers[1])};
HallBus halls; Ssi ssi; WebServer server(80); Preferences preferences;
QueueHandle_t queue; SemaphoreHandle_t snapshotMutex;
std::atomic<uint32_t> owner{0},heartbeatAt{0},barrier{0},stopMask{0};
std::atomic<bool> emergency{true}; // Explicit acknowledgment after every boot.
bool leaseValid() { const uint32_t t=heartbeatAt.load(); return owner.load()!=0&&uint32_t(millis()-t)<=kLeaseMs; }
bool fsReady=false,nvsReady=false;
uint32_t revision=0,nextId=1,bootId=0;

struct Record { uint32_t magic=0x32534e42,version=1; Config cfg[2]; uint32_t crc=0; };
struct Result { uint32_t id=0; char text[64]={}; };
struct Snapshot {
    Config cfg[2]; Sample sample[2]; State state[2]; bool enabled[2],referenced[2],measured[2];
    double target[2],actualCurrent[2],measuredLow[2],measuredHigh[2]; uint32_t cycles[2],revision=0;
    char fault[2][64]; Result results[12];
    bool physicalStop=true,solEnabled=false,solBusy=false; uint8_t solHalls=0;
    uint32_t solSwitches=0,solFailures=0,maxLoopUs=0; bool nvs=false;
} cached;
Result results[12]; unsigned resultIndex=0;
struct Command {
    uint32_t id=0,epoch=0,client=0,expectedRevision=0; char op[24]={};
    unsigned axis=0; double value=0,low=0,high=0;
    uint32_t cycles=1,dwell=500,duration=100; bool relative=false; Config cfg;
};

// Bounded, nonblocking bistable-solenoid runner. GPIO25/26/15 and Hall32/33 preserved.
struct Solenoid {
    bool enabled=false,testing=false; unsigned phase=0,direction=0;
    uint32_t since=0,pulseMs=100,cooldown=500,remaining=0,switches=0,failures=0;
    void off() { digitalWrite(board::SOL_EN,LOW); digitalWrite(board::SOL_A,LOW); digitalWrite(board::SOL_B,LOW); phase=0; testing=false; }
    void release() { off(); enabled=false; }
    void pulse(unsigned dir,uint32_t now) {
        direction=dir; digitalWrite(board::SOL_EN,LOW);
        digitalWrite(board::SOL_A,dir==0); digitalWrite(board::SOL_B,dir==1);
        digitalWrite(board::SOL_EN,HIGH); phase=1; since=now;
    }
    const char* start(unsigned dir,uint32_t duration,uint32_t count,uint32_t dwell,uint32_t now) {
        if(!enabled) return "SOLENOID_DISABLED";
        if(phase) return "SOLENOID_BUSY";
        if(dir>1||duration<20||duration>500||count>10000||dwell<250||dwell>30000) return "INVALID_SOLENOID_TEST";
        pulseMs=duration; cooldown=dwell; remaining=count?count*2:1; testing=count>0; switches=failures=0;
        pulse(dir,now); return nullptr;
    }
    void tick(uint32_t now,bool allowed) {
        if(!allowed) { release(); return; }
        if(phase==1&&uint32_t(now-since)>=pulseMs) { digitalWrite(board::SOL_EN,LOW); phase=2; since=now; }
        if(phase==2) {
            if(!digitalRead(board::SOL_HALL[direction])) { ++switches; phase=3; since=now; }
            else if(uint32_t(now-since)>=1000) { ++failures; off(); }
        }
        if(phase==3&&uint32_t(now-since)>=cooldown) {
            if(--remaining==0) off(); else pulse(1-direction,now);
        }
    }
} sol;

bool save() {
    if(!nvsReady) return false;
    Record r{}; for(unsigned i=0;i<2;++i) r.cfg[i]=axes[i].cfg;
    r.crc=crc32(&r,offsetof(Record,crc));
    if(preferences.putBytes("config",&r,sizeof(r))!=sizeof(r)) return false;
    Record check; return preferences.getBytes("config",&check,sizeof(check))==sizeof(check)&&
        !memcmp(&check,&r,sizeof(r))&&check.crc==crc32(&check,offsetof(Record,crc));
}
bool allOff() { return !axes[0].enabled&&!axes[1].enabled&&!axes[0].busy()&&!axes[1].busy()&&!sol.enabled&&!sol.phase; }
void result(uint32_t id,const char* text) {
    Result& r=results[resultIndex++%12]; r.id=id; snprintf(r.text,sizeof(r.text),"%s",text?text:"OK");
}
void execute(const Command& c,uint32_t now,bool physical) {
    const char* error=nullptr; Axis& a=axes[c.axis];
    if(c.epoch!=barrier.load()||c.client!=owner.load()||!leaseValid()) { result(c.id,"CANCELLED_OR_LEASE_EXPIRED"); return; }
    if(!strcmp(c.op,"config")) {
        if(!allOff()) error="DISABLE_ALL_BEFORE_CONFIG";
        else if(c.expectedRevision!=revision) error="STALE_CONFIGURATION";
        else if((error=validate(c.cfg))==nullptr) {
            Config previous=a.cfg; a.cfg=c.cfg;
            if(!save()) { a.cfg=previous; error="NVS_WRITE_FAILED"; }
            else { ++revision; a.release(); initial[c.axis]=a.cfg;
                if(!drivers[c.axis].configure()&&a.cfg.present) error="SAVED_BUT_DRIVER_NOT_READY"; }
        }
    } else if(!strcmp(c.op,"ack")) {
        if(physical) error="PHYSICAL_STOP_ACTIVE";
        else {
            // Clear requires all configured drivers to be healthy; never enables power.
            for(auto& axis:axes) if(axis.cfg.present&&!axis.sample.healthy) error="RECONFIGURE_OR_CHECK_DRIVER";
            if(!error) { emergency.store(false); for(auto& axis:axes) axis.clear(false); }
        }
    } else if(emergency.load()||physical) error="ESTOP_LATCHED";
    else if(!strcmp(c.op,"move")) error=a.move(c.value,c.relative,false,now);
    else if(!strcmp(c.op,"jog")) error=a.move(c.value,true,true,now);
    else if(!strcmp(c.op,"hold")) error=a.hold();
    else if(!strcmp(c.op,"release")) a.release();
    else if(!strcmp(c.op,"zero")) error=a.zero();
    else if(!strcmp(c.op,"home")) error=a.home(false,now);
    else if(!strcmp(c.op,"scan")) error=a.home(true,now);
    else if(!strcmp(c.op,"test")) error=a.test(c.low,c.high,c.cycles,c.dwell,now);
    else if(!strcmp(c.op,"encoder_ref")) error=a.useEncoder();
    else if(!strcmp(c.op,"encoder_zero")) {
        if(a.busy()||axes[1-c.axis].busy()||sol.phase) error="STOP_ALL_BEFORE_CALIBRATION";
        else if(!a.cfg.encoder||!a.cfg.frameConfirmed||!a.sample.encoderValid) error="ENCODER_PROFILE_NOT_VERIFIED";
        else if((error=a.zero())==nullptr) {
            Config previous=a.cfg; a.cfg.encoderZero=a.sample.encoderCount; a.cfg.zeroValid=1;
            if(!save()) { a.cfg=previous; a.release(); error="NVS_WRITE_FAILED"; }
            else { initial[c.axis]=a.cfg; ++revision; }
        }
    } else if(!strcmp(c.op,"sol_enable")) sol.enabled=true;
    else if(!strcmp(c.op,"sol_release")) sol.release();
    else if(!strcmp(c.op,"sol_pulse")) error=sol.start(c.axis,c.duration,0,c.dwell,now);
    else if(!strcmp(c.op,"sol_test")) error=sol.start(c.axis,c.duration,c.cycles,c.dwell,now);
    else error="UNKNOWN_COMMAND";
    result(c.id,error);
}
void control(void*) {
    uint32_t maxLoop=0; bool expired=true;
    for(;;) {
        const uint32_t started=micros(),now=millis();
        bool physical=digitalRead(board::ESTOP)!=LOW;
        if(physical) emergency.store(true);
        bool lease=leaseValid();
        if(!lease&&!expired) { barrier.fetch_add(1); expired=true; }
        if(lease) expired=false;
        uint32_t stops=stopMask.exchange(0);
        if(stops&1) axes[0].stop(now);
        if(stops&2) axes[1].stop(now);
        if(stops&4) sol.release();
        uint8_t active=0; bool hallOk=halls.read(active);
        for(unsigned i=0;i<2;++i) {
            Sample s=drivers[i].sample(); s.hallHealthy=hallOk; s.halls=(active>>(i*3))&7;
            ssi.read(i,axes[i].cfg,s);
            axes[i].tick(s,millis(),lease,emergency.load()||physical);
        }
        sol.tick(millis(),lease&&!emergency.load()&&!physical);
        Command c;
        // One command per control turn keeps queue load from starving protections.
        if(xQueueReceive(queue,&c,0)==pdTRUE) execute(c,millis(),physical);
        maxLoop=std::max(maxLoop,uint32_t(micros()-started));
        if(uint32_t(micros()-started)>100000&&(!allOff())) {
            emergency.store(true); barrier.fetch_add(1); for(auto& a:axes) a.trip("CONTROL_DEADLINE"); sol.release();
        }
        Snapshot copy{}; copy.revision=revision; copy.physicalStop=physical; copy.nvs=nvsReady;
        copy.solEnabled=sol.enabled; copy.solBusy=sol.phase!=0; copy.solSwitches=sol.switches; copy.solFailures=sol.failures;
        copy.solHalls=(!digitalRead(board::SOL_HALL[0]))|((!digitalRead(board::SOL_HALL[1]))<<1); copy.maxLoopUs=maxLoop;
        for(unsigned i=0;i<2;++i) {
            copy.cfg[i]=axes[i].cfg; copy.sample[i]=axes[i].sample; copy.state[i]=axes[i].state;
            copy.enabled[i]=axes[i].enabled; copy.referenced[i]=axes[i].referenced; copy.target[i]=axes[i].target;
            copy.cycles[i]=axes[i].cycles; copy.actualCurrent[i]=drivers[i].actualCurrent();
            copy.measured[i]=axes[i].measured; copy.measuredLow[i]=axes[i].measuredLow; copy.measuredHigh[i]=axes[i].measuredHigh;
            snprintf(copy.fault[i],64,"%s",axes[i].fault);
        }
        memcpy(copy.results,results,sizeof(results));
        xSemaphoreTake(snapshotMutex,portMAX_DELAY); cached=copy; xSemaphoreGive(snapshotMutex);
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
Snapshot snapshot() { xSemaphoreTake(snapshotMutex,portMAX_DELAY); Snapshot s=cached; xSemaphoreGive(snapshotMutex); return s; }
void send(int code,JsonDocument& doc) { String text; serializeJson(doc,text); server.sendHeader("Cache-Control","no-store"); server.send(code,"application/json",text); }
void fail(int code,const char* message) { StaticJsonDocument<256> d; d["ok"]=false; d["error"]=message; send(code,d); }
bool number(JsonVariantConst v,double lo,double hi,double& n,bool integer=false) {
    if(v.isNull()||v.is<bool>()||!v.is<double>()) return false;
    n=v.as<double>(); return range(n,lo,hi)&&(!integer||n==std::floor(n));
}
template<class T> bool assign(JsonVariantConst v,T& field) {
    double n;
    if(!number(v,double(std::numeric_limits<T>::lowest()),double(std::numeric_limits<T>::max()),n,std::is_integral<T>::value)) return false;
    field=static_cast<T>(n); return true;
}
void getStatus() {
    Snapshot s=snapshot(); DynamicJsonDocument d(20000);
    d["boot"]=bootId; d["owner"]=owner.load(); d["lease_ms"]=kLeaseMs; d["emergency"]=emergency.load();
    d["physical_stop"]=s.physicalStop; d["revision"]=s.revision; d["nvs"]=s.nvs; d["max_loop_us"]=s.maxLoopUs;
    auto arr=d.createNestedArray("axes");
    for(unsigned i=0;i<2;++i) {
        auto a=arr.createNestedObject(); a["state"]=name(s.state[i]); a["enabled"]=s.enabled[i]; a["referenced"]=s.referenced[i]; a["fault"]=s.fault[i];
        const Sample& p=s.sample[i]; a["position"]=p.position; a["target"]=s.target[i]; a["speed"]=p.speed; a["healthy"]=p.healthy;
        a["halls"]=p.halls; a["hall_healthy"]=p.hallHealthy; a["sg"]=p.sg; a["driver"]=p.driver; a["ramp"]=p.ramp;
        a["cycles"]=s.cycles[i]; a["calculated_current_mA"]=s.actualCurrent[i]; a["measured"]=s.measured[i]; a["measured_low"]=s.measuredLow[i]; a["measured_high"]=s.measuredHigh[i];
        char raw[24]; snprintf(raw,sizeof(raw),"%llX",static_cast<unsigned long long>(p.encoderRaw));
        a["encoder_raw_hex"]=raw; a["encoder_valid"]=p.encoderValid; a["encoder_count"]=p.encoderCount; a["encoder_degrees"]=p.encoderDegrees;
        auto o=a.createNestedObject("config"); const Config& c=s.cfg[i];
#define OUT(field) o[#field]=c.field;
        CONFIG_FIELDS(OUT)
#undef OUT
    }
    auto r=d.createNestedArray("results"); for(auto& item:s.results) if(item.id) { auto o=r.createNestedObject(); o["id"]=item.id; o["result"]=item.text; }
    auto solenoid=d.createNestedObject("solenoid"); solenoid["enabled"]=s.solEnabled; solenoid["busy"]=s.solBusy;
    solenoid["halls"]=s.solHalls; solenoid["switches"]=s.solSwitches; solenoid["failures"]=s.solFailures;
    send(200,d);
}
void post() {
    if(server.header("Content-Type").indexOf("application/json")!=0) { fail(415,"JSON_REQUIRED"); return; }
    if(server.arg("plain").length()>6000) { fail(413,"BODY_TOO_LARGE"); return; }
    DynamicJsonDocument d(10000);
    if(deserializeJson(d,server.arg("plain"))) { fail(400,"INVALID_JSON"); return; }
    const char* op=d["op"]|"";
    // Stop bypasses owner, lease and queue; a stale queued move cannot follow it.
    if(!strcmp(op,"estop")||!strcmp(op,"stop")) {
        barrier.fetch_add(1); if(!strcmp(op,"estop")) emergency.store(true);
        stopMask.fetch_or(7); StaticJsonDocument<128> reply; reply["ok"]=true; send(200,reply); return;
    }
    double n; if(!number(d["client"],1,4294967295.0,n,true)) { fail(400,"CLIENT_REQUIRED"); return; }
    uint32_t client=uint32_t(n),now=millis();
    if(!strcmp(op,"claim")) {
        if(owner.load()&&owner.load()!=client&&leaseValid()) { fail(409,"ANOTHER_CONTROLLER"); return; }
        barrier.fetch_add(1); stopMask.fetch_or(7); owner.store(client); heartbeatAt.store(now);
        StaticJsonDocument<128> reply; reply["ok"]=true; send(200,reply); return;
    }
    if(owner.load()!=client||!leaseValid()) { fail(409,"CLAIM_CONTROL_FIRST"); return; }
    if(!strcmp(op,"heartbeat")) { heartbeatAt.store(now); StaticJsonDocument<64> reply; reply["ok"]=true; send(200,reply); return; }
    Command c; c.id=nextId++; c.client=client; c.epoch=barrier.load();
    if(strlen(op)>=sizeof(c.op)) { fail(400,"INVALID_OPERATION"); return; } strcpy(c.op,op);
    if(!number(d["axis"],0,1,n,true)) { fail(400,"AXIS_REQUIRED"); return; } c.axis=unsigned(n);
    Snapshot s=snapshot(); c.cfg=s.cfg[c.axis];
    if(!strcmp(op,"config")) {
        if(!number(d["revision"],0,4294967295.0,n,true)) { fail(400,"REVISION_REQUIRED"); return; } c.expectedRevision=uint32_t(n);
        if(!d["config"].is<JsonObject>()) { fail(400,"CONFIG_REQUIRED"); return; }
        for(JsonPairConst p:d["config"].as<JsonObjectConst>()) {
            bool found=false,valid=false;
#define IN(field) if(!strcmp(p.key().c_str(),#field)) { found=true; valid=assign(p.value(),c.cfg.field); }
            CONFIG_FIELDS(IN)
#undef IN
            if(!found||!valid) { fail(400,"INVALID_CONFIG_FIELD"); return; }
        }
        if(const char* error=validate(c.cfg)) { fail(400,error); return; }
    } else if(!strcmp(op,"move")||!strcmp(op,"jog")) {
        if(!number(d["value"],-350,350,c.value)) { fail(400,"INVALID_ANGLE"); return; }
        if(d.containsKey("relative")&&!d["relative"].is<bool>()) { fail(400,"INVALID_RELATIVE"); return; }
        c.relative=d["relative"]|false;
    } else if(!strcmp(op,"test")) {
        if(!number(d["low"],-170,170,c.low)||!number(d["high"],-170,170,c.high)||!number(d["cycles"],1,10000,n,true)) { fail(400,"INVALID_TEST"); return; }
        c.cycles=uint32_t(n);
        if(!number(d["dwell"],0,30000,n,true)) { fail(400,"INVALID_DWELL"); return; } c.dwell=uint32_t(n);
    } else if(!strcmp(op,"sol_pulse")||!strcmp(op,"sol_test")) {
        if(!number(d["duration"],20,500,n,true)) { fail(400,"INVALID_PULSE"); return; } c.duration=uint32_t(n);
        if(!number(d["dwell"],250,30000,n,true)) { fail(400,"INVALID_COOLDOWN"); return; } c.dwell=uint32_t(n);
        if(!strcmp(op,"sol_test")) { if(!number(d["cycles"],1,10000,n,true)) { fail(400,"INVALID_CYCLES"); return; } c.cycles=uint32_t(n); }
    }
    if(xQueueSend(queue,&c,0)!=pdTRUE) { fail(429,"COMMAND_QUEUE_FULL"); return; }
    StaticJsonDocument<128> reply; reply["ok"]=true; reply["accepted_id"]=c.id; send(202,reply);
}
void setup() {
    board::safeOutputs(); Serial.begin(115200); bootId=esp_random();
    for(auto pin:board::SOL_HALL) pinMode(pin,INPUT_PULLUP);
    queue=xQueueCreate(8,sizeof(Command)); snapshotMutex=xSemaphoreCreateMutex();
    if(!queue||!snapshotMutex) { while(true) delay(1000); }
    nvsReady=preferences.begin("bench-v2",false);
    if(nvsReady&&preferences.getBytesLength("config")==sizeof(Record)) {
        Record r;
        if(preferences.getBytes("config",&r,sizeof(r))==sizeof(r)&&r.magic==0x32534e42&&r.version==1&&r.crc==crc32(&r,offsetof(Record,crc))&&!validate(r.cfg[0])&&!validate(r.cfg[1]))
            for(unsigned i=0;i<2;++i) axes[i].cfg=r.cfg[i];
    }
    SPI.begin(board::SCK,board::MISO,board::MOSI);
    halls.begin(); ssi.begin();
    for(unsigned i=0;i<2;++i) { initial[i]=axes[i].cfg; drivers[i].configure(); }
    if(xTaskCreatePinnedToCore(control,"bench-control",10000,nullptr,3,nullptr,1)!=pdPASS) { while(true) delay(1000); }
    fsReady=LittleFS.begin(false); // Never silently erase user interface on mount failure.
    WiFi.mode(WIFI_AP); WiFi.softAP("Krya-Stand-v2","bench-setup-2026");
    const char* headers[]={"Content-Type"}; server.collectHeaders(headers,1);
    server.on("/api/status",HTTP_GET,getStatus); server.on("/api/command",HTTP_POST,post);
    server.on("/",HTTP_GET,[](){
        if(!fsReady) { server.send(503,"text/plain; charset=utf-8","LittleFS missing. Run pio run -d bench_v2 -t uploadfs. Motors remain disabled."); return; }
        File f=LittleFS.open("/index.html","r"); if(!f) { server.send(503,"text/plain","Missing index.html"); return; }
        server.streamFile(f,"text/html; charset=utf-8"); f.close();
    });
    server.onNotFound([](){server.send(404,"text/plain","Not found");}); server.begin();
    Serial.println("Bench v2: http://192.168.4.1 | AP Krya-Stand-v2 | motors OFF | read docs/bench_v2 first");
}
void loop() { server.handleClient(); delay(2); }
