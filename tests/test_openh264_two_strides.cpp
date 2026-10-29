// Build: g++ -std=c++17 -Isrc -fsanitize=address,undefined -fno-omit-frame-pointer
//         tests/test_openh264_two_strides.cpp -o /tmp/test_openh264_two_strides
// Run: /tmp/test_openh264_two_strides
#include "media/I420PlaneCopy.h"
#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

int main() {
    using dvbstreamer5::media::codec::copyOpenH264I420;
    constexpr int w=8, h=4;
    // SSysMEMBuffer.iStride is an array of TWO integers: Y and shared UV.
    // No third entry can be passed to the typed wrapper.
    const int strides[2] = {12, 7};
    std::vector<std::uint8_t> y(strides[0]*h, 0xee);
    std::vector<std::uint8_t> u(strides[1]*(h/2), 0xee);
    std::vector<std::uint8_t> v(strides[1]*(h/2), 0xee);
    for (int row=0;row<h;++row)
        for (int col=0;col<w;++col)
            y[row*strides[0]+col]=static_cast<std::uint8_t>(10+row*16+col);
    for (int row=0;row<h/2;++row)
        for (int col=0;col<w/2;++col) {
            u[row*strides[1]+col]=static_cast<std::uint8_t>(20+row*16+col);
            v[row*strides[1]+col]=static_cast<std::uint8_t>(130+row*16+col);
        }

    std::vector<std::uint8_t> output;
    assert(copyOpenH264I420(y.data(),u.data(),v.data(),strides,w,h,output));
    assert(output.size()==w*h*3/2);
    for (int row=0;row<h;++row)
        for (int col=0;col<w;++col)
            assert(output[row*w+col]==10+row*16+col);
    for (int row=0;row<h/2;++row)
        for (int col=0;col<w/2;++col) {
            assert(output[w*h+row*w/2+col]==20+row*16+col);
            assert(output[w*h+w*h/4+row*w/2+col]==130+row*16+col);
        }

    // Under-stride and incomplete chroma must fail without changing output.
    const auto previous=output;
    const int invalid[2]={12,3};
    assert(!copyOpenH264I420(y.data(),u.data(),v.data(),invalid,w,h,output));
    assert(!copyOpenH264I420(y.data(),u.data(),nullptr,strides,w,h,output));
    assert(output==previous);
    std::cout << "OpenH264 two-stride I420 copy tests passed\n";
}
