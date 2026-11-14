#include "media/VaapiHrdPolicy.h"

#include <cstdlib>
#include <iostream>

using namespace dvbstreamer5::media::codec;

static void require(bool ok, const char* why) {
    if (!ok) {
        std::cerr << why << '\n';
        std::exit(1);
    }
}

int main() {
    require(!vaapiStrictHrdForStream("stream-A", nullptr), "default off");
    require(!vaapiStrictHrdForStream("stream-A", ""), "empty selector off");
    require(!vaapiStrictHrdForStream("stream-A", "stream-B"), "other tile unchanged");
    require(vaapiStrictHrdForStream("stream-A", "stream-A"), "test tile enabled");
    require(!vaapiStrictHrdForStream("", ""), "empty ID off");
    require(vaapiStrictHrdBufferBits(6000000U) == 2400000U, "6 Mbps 400-ms CPB");
    require(vaapiStrictHrdBufferBits(8000000U) == 3200000U, "8 Mbps 400-ms CPB");
    require(!vaapiHrdFrameExceedsBuffer(250000U, 2400000U),
            "normal frame within CPB");
    require(vaapiHrdFrameExceedsBuffer(512400U, 2400000U),
            "large P-frame must be reported");
    std::cout << "V514 VAAPI per-tile HRD policy: OK\n";
}
