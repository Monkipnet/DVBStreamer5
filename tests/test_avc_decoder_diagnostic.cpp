// Build: g++ -std=c++17 -Isrc tests/test_avc_decoder_diagnostic.cpp -o /tmp/test_avc_decoder_diagnostic
#include "media/AvcDecoderDiagnostic.h"
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <string>

int main() {
    using dvbstreamer5::media::codec::forceOpenH264ForStream;
    const std::string streamId = "stream-1790918496466";
    assert(!forceOpenH264ForStream(nullptr, streamId));
    assert(!forceOpenH264ForStream("", streamId));
    assert(!forceOpenH264ForStream(streamId.c_str(), ""));
    assert(forceOpenH264ForStream(streamId.c_str(), streamId));
    assert(!forceOpenH264ForStream("stream-179091849646", streamId));
    assert(!forceOpenH264ForStream("stream-1790918496466-extra", streamId));
    assert(!forceOpenH264ForStream("stream-other", streamId));

    // Only one tile is affected, not the second one.
    const std::string otherId = "stream-other";
    assert(!forceOpenH264ForStream(streamId.c_str(), otherId));
    std::cout << "Per-stream OpenH264 A/B selection tests passed\n";
}
