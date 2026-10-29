// Build: g++ -std=c++17 -Isrc tests/test_diagnostic_color_i420.cpp -o /tmp/test_diagnostic_color_i420
#include "media/DiagnosticColorI420.h"
#include <cassert>
#include <cstdint>
#include <iostream>

int main() {
    using dvbstreamer5::media::codec::RawVideoFrame;
    using dvbstreamer5::media::codec::fillDiagnosticColorI420;
    RawVideoFrame f;
    f.width=720;
    f.height=576;
    f.i420.resize(static_cast<std::size_t>(f.width)*f.height*3/2, 0);
    f.pts90k=123456;
    f.hasPts=true;
    assert(fillDiagnosticColorI420(f,0));
    const auto ysize=static_cast<std::size_t>(f.width)*f.height;
    const auto* y=f.i420.data();
    const auto* u=y+ysize;
    const auto* v=u+ysize/4;
    const auto cidx=[&](int row,int col) {
        return static_cast<std::size_t>(row/2)*(f.width/2)+col/2;
    };
    assert(u[cidx(10,10)]==128 && v[cidx(10,10)]==128); // neutral
    assert(u[cidx(10,500)]==96 && v[cidx(10,500)]==195); // warm
    assert(u[cidx(400,10)]==192 && v[cidx(400,10)]==80); // blue
    assert(u[cidx(400,500)]==77 && v[cidx(400,500)]==81); // green
    assert(f.pts90k==123456 && f.hasPts);
    const auto first=f.i420;
    assert(fillDiagnosticColorI420(f,100));
    assert(first!=f.i420); // marker moves
    RawVideoFrame bad;
    bad.width=721; bad.height=576;
    bad.i420.assign(500, 99);
    assert(!fillDiagnosticColorI420(bad,0));
    assert(bad.i420[0]==99);
    std::cout << "Color I420 quadrant diagnostic tests passed\n";
}
