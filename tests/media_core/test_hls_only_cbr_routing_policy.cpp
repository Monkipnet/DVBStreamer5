#include "media/HlsOnlyCbrRoutingPolicy.h"

#include <cassert>
#include <iostream>

using dvbstreamer5::media::hls::bypassNetworkCbrForTranscodedHlsOnly;

int main() {
    // Transcoded HLS is a local file segmenter; it must not require the
    // bounded network CBR observer queue when no network output exists.
    assert(bypassNetworkCbrForTranscodedHlsOnly(true, true, false, 0, 0, 0, 0));
    // Never change passthrough or the known-good network output modes.
    assert(!bypassNetworkCbrForTranscodedHlsOnly(false, true, false, 0, 0, 0, 0));
    assert(!bypassNetworkCbrForTranscodedHlsOnly(true, false, false, 0, 0, 0, 0));
    assert(!bypassNetworkCbrForTranscodedHlsOnly(true, true, true, 0, 0, 0, 0));
    assert(!bypassNetworkCbrForTranscodedHlsOnly(true, true, false, 1, 0, 0, 0)); // UDP(-CBR)
    assert(!bypassNetworkCbrForTranscodedHlsOnly(true, true, false, 0, 1, 0, 0)); // SRT
    assert(!bypassNetworkCbrForTranscodedHlsOnly(true, true, false, 0, 0, 1, 0)); // RTSP
    assert(!bypassNetworkCbrForTranscodedHlsOnly(true, true, false, 0, 0, 0, 1)); // RTMP
    std::cout << "HLS-only CBR observer bypass policy OK\n";
    return 0;
}
