// This test has no hardware/network dependencies.
#include "media/NativeVideoSourceCadence.h"
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <string>

using dvbstreamer5::media::codec::NativeVideoSourceCadence;

static void require(bool result,const char* description) {
    if(!result) { std::cerr<<"FAIL source cadence: "<<description<<"\n";std::exit(1); }
}
static std::uint64_t wrap(std::int64_t n) {
    return static_cast<std::uint64_t>(n) & NativeVideoSourceCadence::kMask;
}

int main() {
    constexpr std::uint64_t kBase=1230000ULL;
    NativeVideoSourceCadence source;
    for(int i=0;i<125;++i) {
        // Source compressed PES timestamps are clean 25fps; the decoder can
        // still assign field/reorder PTS that are 1800 ticks early.
        auto result=source.observe(kBase+static_cast<std::uint64_t>(i)*3600ULL,true);
        if(i>=32)require(result.matches25,"25-fps PAL must match");
    }
    require(source.matches25(),"125 decoded PAL pictures must pass the gate");

    // V497: early lock by the fifth compressed PES, not the 33rd.
    // A locked 25-fps source must avoid the first second of spurious FPS
    // gate drops caused by FFmpeg field/reorder PTS.
    source.reset();
    for(int i=0;i<5;++i) {
        auto result=source.observe(kBase+static_cast<std::uint64_t>(i)*3600ULL,true);
        if(i<4)require(!result.matches25,"early PAL lock needs five samples");
        else require(result.matches25,"PAL must lock by five PES");
    }

    // An H.264 B-frame decode-order PES sequence still has 25-fps
    // presentation timestamps, despite 0,3,1,2,4 picture ordering.
    source.reset();
    const int decodeOrder[] = {0,3,1,2,4};
    for(int x : decodeOrder)
        source.observe(kBase+static_cast<std::uint64_t>(x)*3600ULL,true);
    require(source.matches25(),"reordered PAL PES must fast-lock");

    // 29.97/30 and 50fps must NEVER fast-lock and bypass 25fps gate.
    for(std::uint64_t step : {3003ULL,3000ULL,1800ULL}) {
        source.reset();
        for(int i=0;i<5;++i) {
            auto result=source.observe(kBase+static_cast<std::uint64_t>(i)*step,true);
            require(!result.matches25,"fast 30/50fps must use FPS gate");
        }
        source.reset();
        for(int x : decodeOrder) {
            auto result=source.observe(kBase+static_cast<std::uint64_t>(x)*step,true);
            require(!result.matches25,"reordered 30/50fps must use FPS gate");
        }
    }

    // Duplicate/invalid source PTS cannot prove PAL cadence.
    source.reset();
    for(int i=0;i<5;++i)
        source.observe(kBase,true);
    require(!source.matches25(),"duplicate PES timestamps must not fast-lock");

    // Early classifier handles 33-bit wrap and resets on missing PTS.
    source.reset();
    for(int i=0;i<5;++i)
        source.observe(wrap(static_cast<std::int64_t>(NativeVideoSourceCadence::kMask)
                            -7200LL+static_cast<std::int64_t>(i)*3600LL),true);
    require(source.matches25(),"PAL fast-lock survives PTS wrap");
    source.observe(0,false);
    require(!source.matches25(),"missing PTS clears PAL fast-lock");


    // Recreate old behavior: a 1/3 field-PTS offset makes a generic 25fps
    // next-PTS gate reject ~1/3 of correctly decoded real pictures.
    std::uint64_t next=0;
    int dropped=0;
    for(int i=0;i<125;++i) {
        const std::uint64_t rawPts=
            kBase+static_cast<std::uint64_t>(i)*3600ULL-
            ((i%3==1)?1800ULL:0ULL);
        if(i==0)next=rawPts;
        if(rawPts<next) { ++dropped;continue; }
        do {next+=3600;} while(rawPts>=next);
    }
    require(dropped>=30&&dropped<=50,"reordered decoder PTS reproduces ~40 dropped of 125");

    // V610 home2 reproduction: H.264 PES in decode order 0,3,1,2,4
    // causes the old endpoint-span detector to flip 25-fps bypass on/off
    // repeatedly, resetting the gate clock even though source is 25fps.
    // After the long window fills, all B-frame permutations must stay locked.
    const int reorderedPictures[] = {0,3,1,2,4};
    for (std::uint64_t step : {3600ULL,3003ULL,1800ULL}) {
        source.reset();
        bool previouslyLocked=false;
        int falseTransitions=0;
        for(int group=0;group<60;++group){
            for(int picture : reorderedPictures){
                const int presentation=group*5+picture;
                const auto result=source.observe(
                    wrap(static_cast<std::int64_t>(NativeVideoSourceCadence::kMask)
                         -720000LL+static_cast<std::int64_t>(presentation)
                                     *static_cast<std::int64_t>(step)),true);
                if(group>=8 && step==3600ULL && !result.matches25)
                    ++falseTransitions;
                if(group>=8 && step!=3600ULL && result.matches25)
                    require(false,"B-reordered 30/50fps must not bypass FPS gate");
                if(group>=8 && step==3600ULL && !previouslyLocked)
                    require(false,"stable PAL must not lose lock after 33 PES");
                previouslyLocked=result.matches25;
            }
        }
        if(step==3600ULL)
            require(falseTransitions==0,
                    "B-reordered PAL cadence must not toggle bypass");
    }

    // True 29.97/30fps must still use the existing downsampler.
    source.reset();
    for(int i=0;i<125;++i)
        source.observe(kBase+static_cast<std::uint64_t>(i)*3003ULL,true);
    require(!source.matches25(),"29.97fps cannot bypass 25fps gate");

    source.reset();
    for(int i=0;i<125;++i)
        source.observe(kBase+static_cast<std::uint64_t>(i)*1800ULL,true);
    require(!source.matches25(),"50fps cannot bypass 25fps gate");

    // 25fps near 33-bit wrap; source time is modulo 2^33.
    source.reset();
    for(int i=0;i<125;++i)
        source.observe(wrap(static_cast<std::int64_t>(NativeVideoSourceCadence::kMask)-
                            72000LL+static_cast<std::int64_t>(i)*3600LL),true);
    require(source.matches25(),"33-bit wrap stays locked at 25fps");

    // Source bitrate/rate changes require a re-evaluation every picture.
    for(int i=0;i<125;++i)
        source.observe(wrap(10000000LL+static_cast<std::int64_t>(i)*3003LL),true);
    require(!source.matches25(),"25-to-30fps rate change unlocks bypass");

    source.reset();
    for(int i=0;i<33;++i)
        source.observe(kBase+static_cast<std::uint64_t>(i)*3600ULL,true);
    require(source.matches25(),"window locks after 33 PTS samples");
    source.observe(0,false);
    require(!source.matches25(),"missing PTS disables bypass");

    // Inputs faster than 26.5fps remain filtered; slow PAL variants avoid loss.
    source.reset();
    for(int i=0;i<125;++i)
        source.observe(kBase+static_cast<std::uint64_t>(i)*3750ULL,true);
    require(source.matches25(),"24fps input cannot be spuriously gated");

    std::cout<<"PASS verified PAL 25fps PES vs decoder PTS and 30fps safety\n";
    return 0;
}
