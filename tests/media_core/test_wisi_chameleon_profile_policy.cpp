#include "media/WisiChameleonProfilePolicy.h"
#include "media/IpCbrSourceClockPolicy.h"
#include <cstdlib>
#include <iostream>
using dvbstreamer5::media::codec::WisiChameleonProfilePolicy;
static void test(bool ok, const char* msg) {
    if (!ok) { std::cerr << "WISI PROFILE FAIL: " << msg << "\n"; std::exit(1); }
}
int main() {
    using P = WisiChameleonProfilePolicy;
    test(!P::eligible(false,true,true,true,"h264"),"default off");
    test(!P::eligible(true,false,true,true,"h264"),"copy input");
    test(!P::eligible(true,true,false,true,"h264"),"VBR not eligible");
    test(!P::eligible(true,true,true,false,"h264"),"HTTP-only not eligible");
    test(!P::eligible(true,true,true,true,"copy"),"video copy bypass");
    test(P::eligible(true,true,true,true,"h264"),"H264 CBR");
    test(P::eligible(true,true,true,true,"hevc"),"HEVC CBR");
    test(!P::hasTransportHeadroom(4000000,3900000,192000),"oversubscribed");
    test(P::hasTransportHeadroom(8000000,6000000,192000),"MIR24 budget");
    test(P::reserveBitsPerSecond(8000000,192000)==592000,"audio+5% reserve");
    test(P::qsvBufferKB(6000000)==300,"QSV 6Mbps 400ms");
    test(P::qsvBufferKB(8000000)==400,"QSV 8Mbps 400ms");
    test(P::qsvBufferKB(UINT64_MAX)==65535,"QSV extreme bitrate saturates");
    test(P::qsvBufferKB(0)==1,"QSV zero bitrate bounds");
    test(P::nvencVbvFrames(2,25.0)==2,"keep 2-frame NVENC default");
    test(P::nvencVbvFrames(30,25.0)==10,"cap NVENC A/B at 400ms");
    test(P::nvencVbvFrames(30,50.0)==20,"50fps cap");
    // A saved legacy 3-s or 5-s WISI CBR buffer must keep its exact
    // effective bitrate and startup duration when Source-PCR is selected.
    using dvbstreamer5::media::network::sourcePcrCbrTargetKeepingStartupBuffer;
    using dvbstreamer5::media::mpegts::CbrTsPacer;
    for (const auto input : {8000000ULL, 8000705ULL, 8000709ULL}) {
        const auto output = sourcePcrCbrTargetKeepingStartupBuffer(input);
        test(CbrTsPacer::decodeTargetBitrate(output) == 8000000ULL,
             "WISI Source-PCR physical CBR bitrate unchanged");
        test(CbrTsPacer::startupBufferMilliseconds(output) ==
                 CbrTsPacer::startupBufferMilliseconds(input),
             "WISI Source-PCR startup buffer unchanged");
        test(CbrTsPacer::sourcePllEncodedTargetBitrate(output),
             "WISI Source-PCR mode must be selected");
    }
    const auto zeroBuffer = sourcePcrCbrTargetKeepingStartupBuffer(8000699ULL);
    test(CbrTsPacer::startupBufferMilliseconds(zeroBuffer) == 0,
         "explicit zero-buffer preference retained");
    std::cout << "V516 WISI common profile policy: OK\n";
}
