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
