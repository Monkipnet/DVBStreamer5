#include "media/PcrVbrSourceClock.h"
#include <cstdlib>
#include <iostream>
using dvbstreamer5::media::mpegts::Packet;
using dvbstreamer5::media::mpegts::PcrVbrSourceClock;
static void check(bool ok,const char* message){
    if(!ok){std::cerr<<message<<'\n';std::exit(1);}
}
static Packet pkt(std::uint64_t pcr,bool discontinuity=false){
    Packet x{};x[0]=0x47;x[1]=0x01;x[2]=0x00;x[3]=0x20;
    x[4]=7;x[5]=static_cast<std::uint8_t>(0x10|(discontinuity?0x80:0));
    const auto base=pcr/300,ext=pcr%300;
    x[6]=static_cast<std::uint8_t>(base>>25);
    x[7]=static_cast<std::uint8_t>(base>>17);
    x[8]=static_cast<std::uint8_t>(base>>9);
    x[9]=static_cast<std::uint8_t>(base>>1);
    x[10]=static_cast<std::uint8_t>((base&1)<<7|(ext>>8)&1);
    x[11]=static_cast<std::uint8_t>(ext);
    return x;
}
int main(){
    PcrVbrSourceClock clock;
    clock.observe(pkt(0),100);
    check(!clock.ready(),"first PCR must not start sender");
    clock.observe(pkt(2700000),200); // 100 ms / 100 packets
    check(clock.ready(),"second PCR should establish media clock");
    std::int64_t a=0,b=0,c=0;
    check(clock.timeNs(100,a)&&clock.timeNs(150,b)&&clock.timeNs(200,c),
        "PCR interpolation failed");
    check(b-a>=49000000 && b-a<=51000000 &&
          c-a>=99000000 && c-a<=101000000,
          "PCR packet interval not paced at exact source time");
    // Sudden VBR bitrate change: 100 packets across 50 ms, not averaged
    // over preceding slow interval.
    clock.observe(pkt(4050000),300);
    std::int64_t d=0,e=0;
    check(clock.timeNs(250,d)&&clock.timeNs(300,e),
        "VBR source interpolation unavailable");
    check(e-d>=24000000 && e-d<=26000000,
        "VBR rate did not follow local PCR interval");
    // Real T49/T17 feeds legitimately carry PCR steps below one
    // millisecond: 0.4, 0.9 and even repeated PCR. A short step must
    // NOT rebase the 1.2s reservoir or drop the output packet queue.
    const auto beforeResets=clock.resets();
    check(!clock.observe(pkt(4060800),301),
        "0.4ms PCR step caused a false output gap");
    check(!clock.observe(pkt(4074300),302),
        "0.9ms PCR step caused a false output gap");
    check(!clock.observe(pkt(4050000),303),
        "repeated PCR caused a false output gap");
    check(clock.ready() && clock.resets()==beforeResets,
        "sub-ms PCR steps reset the pacing clock");
    check(clock.shortStepsIgnored()==3,
        "valid short PCR steps were not skipped");
    clock.observe(pkt(4320000),320);
    std::int64_t restored=0;
    check(clock.timeNs(320,restored)&&restored>e,
        "PCR timeline failed to recover after skipped anchors");
    // An explicit MPEG-TS discontinuity must still reset the clock.
    check(clock.observe(pkt(27000000,true),400),"PCR reset ignored");
    check(!clock.ready(),"PCR reset should discard stale clock anchors");
    clock.observe(pkt(28350000),500);
    check(clock.ready(),"PCR recovery failed");
    std::cout<<"UDP-VBR PCR source clock: OK\n";
}
