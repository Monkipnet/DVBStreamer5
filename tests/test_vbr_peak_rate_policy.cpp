#include "media/VbrPeakRatePolicy.h"
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>

using dvbstreamer5::media::network::VbrPeakRatePolicy;
using namespace std::chrono_literals;
static void require(bool good, const char* why) {
    if (!good) { std::cerr << "VBR PEAK TEST FAIL: " << why << "\n"; std::exit(1); }
}
int main() {
    constexpr std::uint64_t bps=13384000ULL;
    const auto interval=VbrPeakRatePolicy::spacingNs(1316,bps);
    require(interval>750000 && interval<850000,"1316-byte physical spacing");
    require(VbrPeakRatePolicy::spacingNs(0,bps)==0,"empty datagram");
    require(VbrPeakRatePolicy::spacingNs(1316,0)==0,"disabled pacer");
    require(VbrPeakRatePolicy::spacingNs(static_cast<std::size_t>(-1),bps)>0,
            "overflow saturation");

    using Clock=std::chrono::steady_clock;
    const auto origin=Clock::time_point{};
    require(VbrPeakRatePolicy::canDelay(origin,origin+80ms),
            "80ms safe source-PCR debt");
    require(!VbrPeakRatePolicy::canDelay(origin,origin+81ms),
            "81ms must bypass cap to protect decoder");
    require(!VbrPeakRatePolicy::canDelay(origin,origin),
            "no additional delay");

    // A 17.5-Mbit/s source-PCR burst should be paced at or below the
    // 13.384-Mbit/s ceiling while remaining inside the 80-ms guard.
    auto previous=origin;
    unsigned shaped=0,bypassed=0;
    for (int i=0; i<200; ++i) {
        const auto source=origin+std::chrono::microseconds(i*600);
        auto due=source;
        if (i) {
            const auto candidate=previous+std::chrono::nanoseconds(interval);
            if (VbrPeakRatePolicy::canDelay(source,candidate)) {
                due=candidate;++shaped;
            } else if (candidate>source) {
                ++bypassed;
            }
        }
        previous=due;
    }
    require(shaped>150,"transcoded VBR burst not smoothed");
    require(bypassed==0,"healthy 100ms VBR burst incorrectly bypassed");
    require(previous-origin>150ms,"VBR output was not paced");
    std::cout<<"V518 UDP-VBR rate cap and deadline guard: OK\n";
}
