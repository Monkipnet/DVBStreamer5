#include "media/PcrVbrSourceClock.h"
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
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
    // Source-PCR may jitter backwards by a fraction of a millisecond.
    // Neither a silent clock jump nor output-queue flush is warranted.
    check(!clock.observe(pkt(4309200),321),
        "sub-ms backward PCR jitter caused a false output gap");
    check(clock.resets()==beforeResets && clock.ready(),
        "sub-ms backward PCR jitter reset the pacing clock");
    // An explicit MPEG-TS discontinuity must still reset the clock.
    check(clock.observe(pkt(27000000,true),400),"PCR reset ignored");
    check(!clock.ready(),"PCR reset should discard stale clock anchors");
    check(std::string(clock.lastResetCause())=="discontinuity_flag",
        "discontinuity flag missing from PCR reset diagnostics");
    check(clock.lastResetPreviousPcr()==4320000ULL &&
          clock.lastResetCurrentPcr()==27000000ULL &&
          clock.lastResetPreviousIndex()==320ULL &&
          clock.lastResetCurrentIndex()==400ULL &&
          clock.lastResetPacketGap()==80ULL,
          "PCR discontinuity diagnostic lost source packet indices/ticks");
    clock.observe(pkt(28350000),500);
    check(clock.ready(),"PCR recovery failed");
    // V605 home2 regression: a 7.2-second source jump must expose the
    // exact PCR and packet-index gap, with no change in reset behavior.
    PcrVbrSourceClock forward;
    const auto t0=std::chrono::steady_clock::time_point{};
    check(!forward.observe(pkt(2700000ULL),100,t0),"first PCR reset");
    check(!forward.observe(pkt(5400000ULL),200,t0+std::chrono::milliseconds(100)),
          "second PCR reset");
    check(forward.ready(),"initial forward clock unavailable");
    check(forward.observe(pkt(199800000ULL),260,t0+std::chrono::milliseconds(200)),
          "7.2-second forward PCR jump was not detected");
    check(std::string(forward.lastResetCause())=="forward_jump" &&
          forward.lastResetDeltaMs()==7200,
          "forward PCR jump classification changed");
    check(forward.lastResetPreviousPcr()==5400000ULL &&
          forward.lastResetCurrentPcr()==199800000ULL &&
          forward.lastResetPreviousIndex()==200ULL &&
          forward.lastResetCurrentIndex()==260ULL &&
          forward.lastResetPacketGap()==60ULL,
          "forward PCR jump diagnostics incorrect");
    check(forward.lastResetWallGapMs()==100,
          "forward PCR wall-time gap differs from source packet gap");
    check(!forward.ready(),"real forward jump failed to reset pacing anchors");
    // A real source stall can have the same 7.2-second PCR jump, but a
    // 7.2-second monotonic observation gap: distinguish without changing
    // any packet or the existing reset policy.
    PcrVbrSourceClock stalled;
    check(!stalled.observe(pkt(2700000ULL),100,t0),"initial PCR reset");
    check(!stalled.observe(pkt(5400000ULL),200,t0+std::chrono::milliseconds(100)),
          "second stalled-PCR anchor reset");
    check(stalled.observe(pkt(199800000ULL),260,
                                 t0+std::chrono::milliseconds(7300)),
          "stalled source PCR jump not detected");
    check(stalled.lastResetWallGapMs()==7200,
          "stalled source not distinguished by monotonic PCR gap");
    // Dense DVB PCR schedule exceeds the 512-anchor retention limit.
    // O(log N) lookups must interpolate exactly at anchors and between
    // them, with both forward and backward extrapolation unchanged.
    PcrVbrSourceClock dense;
    for (std::uint64_t i=0; i<1024; ++i)
        check(!dense.observe(pkt(i*270000ULL), i*100ULL),
              "unexpected reset during dense PCR sequence");
    check(dense.ready(),"dense PCR schedule not ready");
    for (const std::uint64_t index :
         {0ULL,51100ULL,51200ULL,51250ULL,70000ULL,70050ULL,
          102200ULL,102300ULL,103000ULL}) {
        std::int64_t ns=0;
        check(dense.timeNs(index,ns),"binary-PCR search failed");
        const std::int64_t expected=
            static_cast<std::int64_t>(index) * 100000LL;
        check(ns==expected,"binary-PCR lookup changed source schedule");
    }
    std::cout<<"UDP-VBR PCR source clock: OK\n";
}
