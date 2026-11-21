#include "media/H264FieldReference.h"

#include <cassert>
#include <cstdint>

int main() {
    for (std::uint8_t first = 0; first < 4; ++first) {
        for (std::uint8_t second = 0; second < 4; ++second) {
            const bool expected = (first == 0) != (second == 0);
            assert(dvbstreamer5::media::mpegts::h264MixedReferenceFields(
                       first, second) == expected);
        }
    }
    return 0;
}
