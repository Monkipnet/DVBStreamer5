// Build: g++ -std=c++17 -Isrc tests/test_diagnostic_i420_frame.cpp -o /tmp/test_diagnostic_i420_frame
// Requires no GPU/codec libraries; tests the post-decoder diagnostic raster.
#include "media/DiagnosticI420Frame.h"
#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

int main() {
    using dvbstreamer5::media::codec::RawVideoFrame;
    using dvbstreamer5::media::codec::fillDiagnosticNeutralI420;
    RawVideoFrame frame;
    frame.width=720;
    frame.height=576;
    frame.i420.assign(static_cast<std::size_t>(frame.width)*frame.height*3/2,0);
    frame.pts90k=270000;
    frame.dts90k=267000;
    frame.hasPts=frame.hasDts=true;
    assert(fillDiagnosticNeutralI420(frame,0));

    const auto ySize=static_cast<std::size_t>(frame.width)*frame.height;
    const auto chroma=ySize/4;
    for(std::size_t i=ySize;i<ySize+chroma*2;++i) assert(frame.i420[i]==128);
    assert(frame.i420[4*frame.width+4]==144);
    assert(frame.i420[64*frame.width+4]==56);
    assert(frame.i420[4*frame.width+64]==56);
    assert(frame.pts90k==270000 && frame.dts90k==267000);
    assert(frame.width==720 && frame.height==576);
    assert(frame.hasPts && frame.hasDts);

    const auto first=frame.i420;
    assert(fillDiagnosticNeutralI420(frame,13));
    assert(first!=frame.i420); // moving white marker

    RawVideoFrame invalid;
    invalid.width=720;
    invalid.height=576;
    invalid.i420.assign(100,0);
    const auto previous=invalid.i420;
    assert(!fillDiagnosticNeutralI420(invalid,0));
    assert(invalid.i420==previous);

    std::cout << "Post-decoder neutral I420 diagnostic tests passed\n";
}
