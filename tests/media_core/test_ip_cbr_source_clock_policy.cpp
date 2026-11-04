#include "media/IpCbrSourceClockPolicy.h"
#include "media/CbrTsPacer.h"

#include <cstdlib>
#include <iostream>
#include <string>

using dvbstreamer5::media::network::enableIpCbrSourceClockAb;
using dvbstreamer5::media::mpegts::CbrTsPacer;

static void require(bool ok, const char* description) {
    if (!ok) {
        std::cerr << "IP CBR source-clock test failed: " << description << '\n';
        std::exit(1);
    }
}

int main() {
    const std::string id="stream-chanson";
    for (const auto* input : {"http://example.test/live.ts",
                              "https://example.test/live.ts",
                              "srt://example.test:9000?mode=caller"}) {
        require(enableIpCbrSourceClockAb(
                    id.c_str(),id,input,false,true,true,true),
                "exact selected HTTP/SRT passthrough CBR");
        require(!enableIpCbrSourceClockAb(
                    "other-channel",id,input,false,true,true,true),
                "unrelated channel unchanged");
        require(!enableIpCbrSourceClockAb(
                    nullptr,id,input,false,true,true,true),
                "default disabled");
        require(!enableIpCbrSourceClockAb(
                    id.c_str(),id,input,true,true,true,true),
                "transcoding legacy policy unchanged");
        require(!enableIpCbrSourceClockAb(
                    id.c_str(),id,input,false,false,true,true),
                "non-CBR unchanged");
        require(!enableIpCbrSourceClockAb(
                    id.c_str(),id,input,false,true,false,true),
                "no UDP-CBR unchanged");
        require(!enableIpCbrSourceClockAb(
                    id.c_str(),id,input,false,true,true,false),
                "non-whole-kbps target unchanged");
    }
    for (const auto* input : {"dvb://satellite?adapter=0",
                              "udp://239.0.0.1:1234",
                              "external://hls", "file:///tmp/a.ts"}) {
        require(!enableIpCbrSourceClockAb(
                    id.c_str(),id,input,false,true,true,true),
                "other protocols unchanged");
    }
    // +777 is the already-existing WISI source-PCR profile with 3-s startup
    // buffering and without +809 reservoir rate correction.
    require(CbrTsPacer::sourcePllEncodedTargetBitrate(4000777ULL),
            "source PCR marker active");
    require(CbrTsPacer::decodeTargetBitrate(4000777ULL)==4000000ULL,
            "exact four-megabit physical clock");
    require(CbrTsPacer::startupBufferMilliseconds(4000777ULL)==3000ULL,
            "default startup reservoir retained");
    require(!CbrTsPacer::startupBufferEncodedTargetBitrate(4000777ULL),
            "no opt-in 500-ms stepping side effect");
    std::cout << "IP CBR source clock policy OK\n";
}
