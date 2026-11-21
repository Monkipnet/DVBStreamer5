#include "media/TsAvStageTrace.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <sstream>
#include <string>
#include <iostream>

using dvbstreamer5::media::mpegts::TsAvStageTrace;

static std::array<std::uint8_t,188> pes(std::uint16_t pid,
                                        std::uint8_t streamId,
                                        std::uint64_t pts,
                                        bool scrambled=false) {
    std::array<std::uint8_t,188> p{};
    p.fill(0xff);
    p[0] = 0x47;
    p[1] = static_cast<std::uint8_t>(0x40U | (pid >> 8U));
    p[2] = static_cast<std::uint8_t>(pid);
    p[3] = scrambled ? 0x90U : 0x10U;
    p[4] = 0x00; p[5] = 0x00; p[6] = 0x01; p[7] = streamId;
    p[8] = 0x00; p[9] = 0x00;
    p[10] = 0x80; p[11] = 0x80; p[12] = 0x05;
    p[13] = static_cast<std::uint8_t>(0x21U | ((pts >> 29U) & 0x0eU));
    p[14] = static_cast<std::uint8_t>(pts >> 22U);
    p[15] = static_cast<std::uint8_t>(0x01U | ((pts >> 14U) & 0xfeU));
    p[16] = static_cast<std::uint8_t>(pts >> 7U);
    p[17] = static_cast<std::uint8_t>(0x01U | ((pts << 1U) & 0xfeU));
    return p;
}

int main() {
    TsAvStageTrace trace("sat-129", "post_ca", 767, 770);
    const auto video = pes(767, 0xe0, 90000);
    const auto audio0 = pes(770, 0xbd, 90000);
    const auto audio1 = pes(770, 0xbd, 120000);
    const auto encrypted = pes(770, 0xbd, 125000, true);
    auto bad = pes(770, 0xbd, 130000);
    bad[6] = 0x00; // clear target audio PUSI with broken PES prefix
    const auto unrelated = pes(888, 0xbd, 150000);

    trace.inspect(video.data(), video.size());
    trace.inspect(audio0.data(), audio0.size());
    trace.inspect(audio1.data(), audio1.size());
    trace.inspect(encrypted.data(), encrypted.size());
    trace.inspect(bad.data(), bad.size());
    trace.inspect(unrelated.data(), unrelated.size());

    const auto& v = trace.stats(false);
    const auto& a = trace.stats(true);
    assert(v.packets == 1 && v.clearPes == 1 && v.pts == 1);
    assert(a.packets == 4 && a.pusi == 4);
    assert(a.scrambled == 1 && a.clearPes == 2 && a.private1Pes == 2);
    assert(a.badPes == 1 && a.pts == 2 && a.ptsForwardOver200 == 1);
    std::ostringstream output;
    auto* old = std::cerr.rdbuf(output.rdbuf());
    trace.report();
    std::cerr.rdbuf(old);
    assert(output.str().find("stage=post_ca kind=audio pid=770") != std::string::npos);
    assert(output.str().find("private1_pes=2") != std::string::npos);
    assert(output.str().find("bad_pes=1") != std::string::npos);
    return 0;
}
