// Run: g++ -std=c++17 -Isrc tests/test_native_scale_mode.cpp -o /tmp/test_native_scale_mode && /tmp/test_native_scale_mode
#include "media/NativeScaleMode.h"
#include <cassert>
#include <cstdlib>
#include <iostream>

int main() {
#if defined(__linux__)
    unsetenv("DVBSTREAMER5_FORCE_CPU_SCALE");
    assert(!dvbstreamer5::media::codec::forceCpuVideoScaling());
    setenv("DVBSTREAMER5_FORCE_CPU_SCALE", "0", 1);
    assert(!dvbstreamer5::media::codec::forceCpuVideoScaling());
    setenv("DVBSTREAMER5_FORCE_CPU_SCALE", "1", 1);
    assert(dvbstreamer5::media::codec::forceCpuVideoScaling());
    setenv("DVBSTREAMER5_FORCE_CPU_SCALE", "true", 1);
    assert(!dvbstreamer5::media::codec::forceCpuVideoScaling());
    unsetenv("DVBSTREAMER5_FORCE_CPU_SCALE");
#endif
    std::cout << "CPU scaling opt-in tests passed\n";
}
