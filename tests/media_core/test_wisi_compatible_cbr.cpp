#include "media/CbrTsPacer.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
using namespace dvbstreamer5::media::mpegts;

static unsigned pidOf(const Packet& p) { return ((p[1]&31U)<<8U)|p[2]; }
static Packet source(unsigned i) {
    Packet p{};p.fill(0xff);p[0]=0x47;p[1]=0x01;p[2]=0x02;
    const bool hasPcr = i%45U==0;
    p[3]=static_cast<std::uint8_t>((hasPcr?0x30U:0x10U)|(i&15U));
    if (hasPcr) {
        const std::uint64_t base=(27000000ULL+(i/45U)*540000ULL)/300ULL;
        p[4]=7;p[5]=0x10;
        p[6]=static_cast<std::uint8_t>(base>>25U);
        p[7]=static_cast<std::uint8_t>(base>>17U);
        p[8]=static_cast<std::uint8_t>(base>>9U);
        p[9]=static_cast<std::uint8_t>(base>>1U);
        p[10]=static_cast<std::uint8_t>(((base&1U)<<7U)|0x7eU);
        p[11]=0;
    }
    return p;
}
static bool hasPcr(const Packet& p) {
    return (p[3]&0x30U)>=0x20U && p[4]>=7U && (p[5]&0x10U);
}
static std::uint64_t ticks(const Packet& p) {
    const std::uint64_t base=(std::uint64_t(p[6])<<25U)|
        (std::uint64_t(p[7])<<17U)|(std::uint64_t(p[8])<<9U)|
        (std::uint64_t(p[9])<<1U)|(std::uint64_t(p[10])>>7U);
    return base*300ULL+((std::uint64_t(p[10]&1U)<<8U)|p[11]);
}
int main(){
    CbrTsPacer legacy(7500801ULL), compatible(7500801ULL,true);
    assert(!legacy.wisiCompatible() && compatible.wisiCompatible());
    assert(legacy.targetBitrate()==7500000ULL && compatible.targetBitrate()==7500000ULL);
    assert(legacy.maxQueuedBytes()==8ULL*1024ULL*1024ULL);
    assert(compatible.maxQueuedBytes()==32ULL*1024ULL*1024ULL);
    for(unsigned i=0;i<500;++i){
        const Packet p=source(i);
        assert(legacy.enqueue(p));assert(compatible.enqueue(p));
    }
    unsigned legacyPcr=0,syntheticPcr=0,adaptationOnly=0;
    std::uint64_t lastTicks=0,lastSlot=0,slot=0;
    bool started=false;
    for(unsigned i=0;i<800;++i){
        CbrDatagram a{},b{};
        assert(legacy.emitDatagram(std::chrono::steady_clock::now(),a));
        assert(compatible.emitDatagram(std::chrono::steady_clock::now(),b));
        for(unsigned n=0;n<7;++n,++slot){
            if(pidOf(a[n])==258 && hasPcr(a[n]))++legacyPcr;
            if(pidOf(b[n])!=258 || !hasPcr(b[n]))continue;
            const auto current=ticks(b[n]);
            if(started){assert(current>=lastTicks);assert(slot-lastSlot<=115ULL);}
            started=true;lastTicks=current;lastSlot=slot;++syntheticPcr;
            if((b[n][3]&0x30U)==0x20U)++adaptationOnly;
        }
    }
    assert(legacyPcr==12);
    assert(adaptationOnly>=40 && syntheticPcr>legacyPcr);
    std::cout << "wisi_compatible PCR-only cadence and legacy off: OK\n";
}
