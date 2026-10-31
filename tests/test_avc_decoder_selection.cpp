// Build: g++ -std=c++17 -Isrc tests/test_avc_decoder_selection.cpp -o /tmp/test_avc_decoder_selection
#include "media/AvcDecoderSelection.h"
#include <cassert>
#include <iostream>
#include <string>

int main() {
    using dvbstreamer5::media::codec::chooseAvcDecoder;
    const std::string target = "stream-1790918496466";
    assert(chooseAvcDecoder("auto",target,nullptr)=="auto");
    assert(chooseAvcDecoder("auto",target,"")=="auto");
    assert(chooseAvcDecoder("auto",target,"stream-other")=="auto");
    assert(chooseAvcDecoder("auto",target,target.c_str())=="openh264");

    // Explicit per-stream settings always override the environment and
    // survive without a diagnostic drop-in.
    assert(chooseAvcDecoder("openh264",target,nullptr)=="openh264");
    assert(chooseAvcDecoder("openh264",target,"stream-other")=="openh264");
    assert(chooseAvcDecoder("ittiam",target,nullptr)=="ittiam");
    assert(chooseAvcDecoder("ittiam",target,target.c_str())=="ittiam");
    assert(chooseAvcDecoder("ffmpeg",target,nullptr)=="ffmpeg");
    assert(chooseAvcDecoder("ffmpeg",target,target.c_str())=="ffmpeg");
    assert(chooseAvcDecoder("intel_hw",target,nullptr)=="intel_hw");
    assert(chooseAvcDecoder("intel_hw",target,target.c_str())=="intel_hw");
    assert(chooseAvcDecoder("nvidia_hw",target,nullptr)=="nvidia_hw");
    assert(chooseAvcDecoder("nvidia_hw",target,target.c_str())=="nvidia_hw");

    // Old/future unsupported settings are treated as Auto. Never force
    // OpenH264 globally or for a different stream.
    assert(chooseAvcDecoder("unknown",target,nullptr)=="auto");
    assert(chooseAvcDecoder("auto","stream-other",target.c_str())=="auto");
    std::cout << "Per-tile AVC decoder selection tests passed\n";
}
