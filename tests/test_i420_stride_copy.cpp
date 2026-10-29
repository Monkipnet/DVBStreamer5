// Run: g++ -std=c++17 -Isrc tests/test_i420_stride_copy.cpp -o /tmp/test_i420_stride_copy && /tmp/test_i420_stride_copy
// Simulates differently padded Y, U and V rows from a decoder.
#include "media/I420PlaneCopy.h"
#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

int main() {
    const int w=8,h=4;
    const int sy=12,su=6,sv=9;
    std::vector<std::uint8_t> y(sy*h, 0xee);
    std::vector<std::uint8_t> u(su*(h/2), 0xee);
    std::vector<std::uint8_t> v(sv*(h/2), 0xee);
    for (int row=0;row<h;++row)
        for (int x=0;x<w;++x) y[row*sy+x]=static_cast<std::uint8_t>(row*16+x);
    for (int row=0;row<h/2;++row)
        for (int x=0;x<w/2;++x) {
            u[row*su+x]=static_cast<std::uint8_t>(20+row*8+x);
            v[row*sv+x]=static_cast<std::uint8_t>(120+row*8+x);
        }

    std::vector<std::uint8_t> out;
    using dvbstreamer5::media::codec::copyDecoderI420;
    assert(copyDecoderI420(y.data(),sy,u.data(),su,v.data(),sv,w,h,out));
    assert(out.size()==static_cast<std::size_t>(w*h*3/2));
    for(int row=0;row<h;++row)
        for(int x=0;x<w;++x) assert(out[row*w+x]==row*16+x);
    for(int row=0;row<h/2;++row)
        for(int x=0;x<w/2;++x){
            assert(out[w*h+row*(w/2)+x]==20+row*8+x);
            assert(out[w*h+w*h/4+row*(w/2)+x]==120+row*8+x);
        }

    // Malformed strides and odd dimensions must be rejected without changing output.
    const auto previous=out;
    assert(!copyDecoderI420(y.data(),sy,u.data(),su,v.data(),w/2-1,w,h,out));
    assert(!copyDecoderI420(y.data(),sy,u.data(),su,v.data(),sv,w-1,h,out));
    assert(!copyDecoderI420(y.data(),sy,u.data(),su,nullptr,sv,w,h,out));
    assert(out==previous);

    // Each plane may be tightly packed as well.
    std::vector<std::uint8_t> packed;
    assert(copyDecoderI420(out.data(),w,out.data()+w*h,w/2,
         out.data()+w*h+w*h/4,w/2,w,h,packed));
    assert(packed==out);

    std::cout<<"I420 independent-plane stride copy tests passed\n";
}
