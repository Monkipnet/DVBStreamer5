#include "media/TsAvStageTrace.h"
#include <array>
#include <cassert>
#include <cstdint>
#include <sstream>
#include <string>
#include <iostream>

using dvbstreamer5::media::mpegts::TsAvStageTrace;
using Packet = std::array<std::uint8_t, 188>;

static Packet make(std::uint16_t pid, std::uint8_t cc,
                   std::uint8_t sc, std::uint8_t streamId) {
    Packet p{};
    p.fill(0xff);
    p[0] = 0x47;
    p[1] = static_cast<std::uint8_t>(0x40U | ((pid >> 8U) & 0x1fU));
    p[2] = static_cast<std::uint8_t>(pid);
    p[3] = static_cast<std::uint8_t>(0x10U | (cc & 0x0fU) |
                                      ((sc & 3U) << 6U));
    p[4] = 0; p[5] = 0; p[6] = 1; p[7] = streamId;
    p[8] = 0; p[9] = 0; p[10] = 0x80;
    p[11] = 0x80; p[12] = 5;
    p[13] = 0x21; p[14] = 0; p[15] = 1; p[16] = 0; p[17] = 1;
    return p;
}

int main() {
    TsAvStageTrace tr("sat-129", "post_ca", 767, 770);
    const auto v = make(767, 0, 0, 0xe0);
    tr.inspect(v.data(), v.size());

    auto brokenPrefix = make(767, 1, 0, 0xe0);
    brokenPrefix[4] = 0x76;
    tr.inspect(brokenPrefix.data(), brokenPrefix.size());

    const auto wrongStream = make(767, 2, 0, 0xbd);
    tr.inspect(wrongStream.data(), wrongStream.size());

    auto tooShort = make(767, 3, 0, 0xe0);
    tooShort[3] = 0x30; // adaptation + payload
    tooShort[4] = 178; // leaves only 5 bytes for PES
    tr.inspect(tooShort.data(), tooShort.size());

    auto invalidAdapt = make(767, 4, 0, 0xe0);
    invalidAdapt[3] = 0x30;
    invalidAdapt[4] = 183; // no room for a payload on AFC=3
    tr.inspect(invalidAdapt.data(), invalidAdapt.size());

    const auto a = make(770, 0, 0, 0xbd);
    tr.inspect(a.data(), a.size());
    const auto even = make(770, 1, 2, 0xbd);
    const auto odd = make(770, 2, 3, 0xbd);
    const auto reserved = make(770, 3, 1, 0xbd);
    tr.inspect(even.data(), even.size());
    tr.inspect(odd.data(), odd.size());
    tr.inspect(reserved.data(), reserved.size());

    const auto& vs = tr.stats(false);
    const auto& as = tr.stats(true);
    assert(vs.packets == 5 && vs.clearPes == 1 && vs.badPes == 4);
    assert(vs.badPesPrefix == 1 && vs.badPesStreamId == 1);
    assert(vs.badPesTruncated == 1 && vs.badPesAdaptation == 1);
    assert(as.clearPes == 1 && as.private1Pes == 1);
    assert(as.scrambled == 3 && as.scrambledEven == 1);
    assert(as.scrambledOdd == 1 && as.scrambledReserved == 1);
    assert(as.badPes == 0);

    std::ostringstream log;
    auto* old = std::cerr.rdbuf(log.rdbuf());
    tr.report();
    std::cerr.rdbuf(old);
    const auto text = log.str();
    assert(text.find("bad_pes_prefix_delta=1") != std::string::npos);
    assert(text.find("bad_pes_stream_delta=1") != std::string::npos);
    assert(text.find("bad_pes_short_delta=1") != std::string::npos);
    assert(text.find("bad_pes_adaptation_delta=1") != std::string::npos);
    assert(text.find("scramble_even_delta=1") != std::string::npos);
    assert(text.find("scramble_odd_delta=1") != std::string::npos);
    assert(text.find("scramble_reserved_delta=1") != std::string::npos);
    return 0;
}
