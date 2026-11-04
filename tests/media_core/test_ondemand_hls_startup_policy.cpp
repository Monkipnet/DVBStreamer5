#include "media/OnDemandHlsStartupPolicy.h"

#include <cassert>
#include <chrono>
#include <iostream>

int main() {
    using namespace std::chrono;
    using namespace dvbstreamer5::media::ondemand;
    const auto start = steady_clock::time_point{} + seconds(100);

    assert(keepTranscodedHlsOnDemandStarting(start,start,false));
    assert(keepTranscodedHlsOnDemandStarting(start,start+seconds(30),false));
    assert(keepTranscodedHlsOnDemandStarting(start,start+seconds(90),false));
    assert(keepTranscodedHlsOnDemandStarting(start,start+seconds(119),false));
    assert(!keepTranscodedHlsOnDemandStarting(start,start+seconds(120),false));
    assert(!keepTranscodedHlsOnDemandStarting(start,start+seconds(121),false));
    assert(!keepTranscodedHlsOnDemandStarting(start,start+seconds(1),true));
    assert(!keepTranscodedHlsOnDemandStarting(
        steady_clock::time_point{},start,false));
    assert(!keepTranscodedHlsOnDemandStarting(start,start-seconds(1),false));

    std::cout << "OnDemand HLS cold-start policy OK\n";
    return 0;
}
