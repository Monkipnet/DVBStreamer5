#include "media/IpCbrSourceClockPolicy.h"

#include <cstdlib>
#include <iostream>
#include <string>

using dvbstreamer5::media::network::enableIpCbrSourceClock;
using dvbstreamer5::media::network::sourcePcrCbrTargetKeepingStartupBuffer;
using dvbstreamer5::media::mpegts::CbrTsPacer;

static void require(bool ok, const char* description) {
    if (!ok) {
        std::cerr << "IP CBR source-clock test failed: " << description << '\n';
        std::exit(1);
    }
}

int main() {
    for (const auto* input : {"http://example.test/live.ts",
                              "https://example.test/live.ts",
                              "srt://example.test:9000?mode=caller"}) {
        require(enableIpCbrSourceClock(
                    true,input,false,true,true,true),
                "enabled HTTP/SRT passthrough CBR");
        require(!enableIpCbrSourceClock(
                    false,input,false,true,true,true),
                "default off and unrelated tiles unchanged");
        require(!enableIpCbrSourceClock(
                    true,input,true,true,true,true),
                "transcoding unchanged");
        require(!enableIpCbrSourceClock(
                    true,input,false,false,true,true),
                "non-CBR unchanged");
        require(!enableIpCbrSourceClock(
                    true,input,false,true,false,true),
                "non-UDP-CBR unchanged");
        require(!enableIpCbrSourceClock(
                    true,input,false,true,true,false),
                "non-whole-kbps bitrate unchanged");
        require(!enableIpCbrSourceClock(
                    true,input,false,true,true,true,true),
                "test pattern unchanged");
        require(!enableIpCbrSourceClock(
                    true,input,false,true,true,true,false,true),
                "HLS mode unchanged");
    }
    for (const auto* input : {"dvb://satellite?adapter=0",
                              "udp://239.0.0.1:1234",
                              "external://hls", "file:///tmp/a.ts"}) {
        require(!enableIpCbrSourceClock(
                    true,input,false,true,true,true),
                "other input protocols unchanged");
    }

    // Preserve each existing startup-buffer mode while changing PCR policy.
    for (const auto configured : {4000000ULL,4000699ULL,4000700ULL,
                                  4000703ULL,4000705ULL,4000729ULL,
                                  4000777ULL,4000799ULL,4000800ULL,
                                  4000803ULL,4000829ULL}) {
        const auto converted=sourcePcrCbrTargetKeepingStartupBuffer(configured);
        require(CbrTsPacer::sourcePllEncodedTargetBitrate(converted),
                "source-PCR marker activated");
        require(CbrTsPacer::decodeTargetBitrate(converted)==4000000ULL,
                "physical UDP bitrate unchanged");
        require(CbrTsPacer::startupBufferMilliseconds(converted)==
                    CbrTsPacer::startupBufferMilliseconds(configured),
                "selected startup reservoir preserved");
    }
    std::cout << "IP CBR GUI source-clock policy tests OK\n";
    return 0;
}
