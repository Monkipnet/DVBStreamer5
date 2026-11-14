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

    // A real HLS .ts/.m4s delivery, not a playlist probe, renews a
    // bounded viewer grace. At expiry an idle stream is released.
    const auto media = start + seconds(15);
    assert(keepHlsOnDemandFromRecentMedia(media, media));
    assert(keepHlsOnDemandFromRecentMedia(media, media + seconds(30)));
    assert(keepHlsOnDemandFromRecentMedia(media, media + seconds(74)));
    assert(!keepHlsOnDemandFromRecentMedia(media, media + seconds(75)));
    assert(!keepHlsOnDemandFromRecentMedia(media, media + seconds(120)));
    assert(!keepHlsOnDemandFromRecentMedia(media, media - seconds(1)));
    assert(!keepHlsOnDemandFromRecentMedia(steady_clock::time_point{}, media));

    std::cout << "OnDemand HLS cold-start and viewer-grace policy OK\n";
    return 0;
}
