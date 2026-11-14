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
    test(!P::cbrEligible(false,true,true,"h264"),"non-transcoded CBR bypass");
    test(!P::cbrEligible(true,false,true,"h264"),"non-CBR bypass");
    test(!P::cbrEligible(true,true,false,"h264"),"non-UDP CBR bypass");
    test(!P::cbrEligible(true,true,true,"copy"),"video copy bypass");
    test(P::cbrEligible(true,true,true,"h264"),"automatic H264 CBR");
    test(P::cbrEligible(true,true,true,"hevc"),"automatic HEVC CBR");
    test(!P::vbrEligible(false,true,"h264"),"VBR source copy bypass");
    test(!P::vbrEligible(true,false,"h264"),"non-UDP VBR bypass");
    test(!P::vbrEligible(true,true,"copy"),"video copy in VBR bypass");
    test(P::vbrEligible(true,true,"h264"),"automatic H264 VBR");
    test(P::vbrEligible(true,true,"hevc"),"automatic HEVC VBR");
    test(P::vbrPeakBitsPerSecond(6000000ULL,192000ULL) == 13384000ULL,
         "automatic physical UDP-VBR peak ceiling");
    test(P::vbrPeakBitsPerSecond(UINT64_MAX,192000ULL) == 0ULL,
         "invalid bitrate disables VBR limiter");
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
