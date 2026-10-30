#include "TsIntegrityMonitor.h"
#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>

using Monitor = dvbstreamer5::ca::newcamd::TsIntegrityMonitor;
using Packet = std::array<std::uint8_t, 188>;

static Packet ts(std::uint16_t pid, unsigned cc, unsigned tsc = 2,
                 bool pusi = false, unsigned afc = 1) {
    Packet p{};
    p.fill(0xFF);
    p[0] = 0x47;
    p[1] = static_cast<std::uint8_t>(((pid >> 8) & 0x1F) | (pusi ? 0x40 : 0));
    p[2] = static_cast<std::uint8_t>(pid);
    p[3] = static_cast<std::uint8_t>((tsc << 6) | (afc << 4) | cc);
    if (afc == 2 || afc == 3) { p[4] = 1; p[5] = 0; }
    return p;
}

static void setPcr(Packet& p, std::uint64_t base) {
    p[4] = 7;
    p[5] = 0x10;
    p[6] = static_cast<std::uint8_t>(base >> 25);
    p[7] = static_cast<std::uint8_t>(base >> 17);
    p[8] = static_cast<std::uint8_t>(base >> 9);
    p[9] = static_cast<std::uint8_t>(base >> 1);
    p[10] = static_cast<std::uint8_t>(((base & 1U) << 7) | 0x7E);
    p[11] = 0;
}

static void pes(Packet& p, std::uint8_t nalHeader, bool hevc = false) {
    const std::uint8_t prefix[] = {
        0, 0, 1, 0xE0, 0, 0, 0x80, 0x80, 0,
        0, 0, 1, nalHeader, 1
    };
    (void)hevc;
    for (std::size_t i = 0; i < sizeof(prefix); ++i) p[4+i] = prefix[i];
}

int main() {
    Monitor m;
    Monitor differentService;
    constexpr std::uint16_t video = 0x101;
    constexpr std::uint16_t audio = 0x102;
    m.select(video, 0x1B);
    m.select(audio, 0);

    // No selected PID means no counting or leaking other services.
    auto unrelated = ts(0x303, 0);
    m.before(unrelated.data());
    m.after(unrelated.data());
    assert(m.counters().in_packets == 0);

    auto p0 = ts(video, 0, 2);
    auto p1 = ts(video, 1, 2);
    auto p2 = ts(video, 2, 3);
    auto p4 = ts(video, 4, 3);
    m.before(p0.data());
    m.before(p1.data());
    m.before(p1.data()); // same payload CC: count repeat, not a gap.
    m.before(p2.data()); // EVEN -> ODD.
    m.before(p4.data()); // missing CC 3.
    assert(m.counters().in_cc_repeats == 1);
    assert(m.counters().in_cc_gaps == 1);
    assert(m.counters().parity_changes == 1);

    // Adaptation-only packet does not advance payload continuity.
    auto adaptOnly = ts(video, 4, 0, false, 2);
    m.before(adaptOnly.data());
    auto p5 = ts(video, 5);
    m.before(p5.data());
    assert(m.counters().in_cc_gaps == 1);

    // Discontinuity indicator explicitly licenses a CC discontinuity.
    auto discontinuity = ts(video, 13, 0, false, 3);
    discontinuity[5] = 0x80;
    m.before(discontinuity.data());
    assert(m.counters().in_discontinuities == 1);
    assert(m.counters().in_cc_gaps == 1);

    auto tei = ts(video, 14);
    tei[1] |= 0x80;
    m.before(tei.data());
    auto badAdaptation = ts(video, 15, 0, false, 3);
    badAdaptation[4] = 184;
    m.before(badAdaptation.data());
    assert(m.counters().in_tei == 1);
    assert(m.counters().in_bad_afc == 1);

    // Source PCR anomaly detection; explicit discontinuity suppresses
    // PCR continuity checks, just as for TS CC.
    auto pcr0 = ts(video, 0, 0, false, 3);
    setPcr(pcr0, 100000);
    m.before(pcr0.data());
    auto pcrBack = ts(video, 1, 0, false, 3);
    setPcr(pcrBack, 99000);
    m.before(pcrBack.data());
    assert(m.counters().in_pcr_backwards == 1);
    auto pcrJump = ts(video, 2, 0, false, 3);
    setPcr(pcrJump, 300000);
    m.before(pcrJump.data());
    assert(m.counters().in_pcr_jump_gt_1s == 1);

    auto missingCw = ts(video, 0, 2);
    m.after(missingCw.data());
    m.missingKey();
    assert(m.counters().post_still_scrambled == 1);
    assert(m.counters().missing_key_packets == 1);

    // The original scrambled header becomes clear in the production CSA path.
    auto goodPes = ts(video, 1, 0, true);
    pes(goodPes, 0x65);
    m.after(goodPes.data());
    assert(m.counters().post_pes_ok == 1);
    assert(m.counters().post_video_nal_headers == 1);
    assert(m.counters().post_video_nal_bad == 0);

    auto badNal = ts(video, 2, 0, true);
    pes(badNal, 0x80); // forbidden bit set / H264 type 0
    m.after(badNal.data());
    assert(m.counters().post_video_nal_bad == 1);

    auto badPes = ts(video, 3, 0, true);
    badPes[4] = 0xDE;
    m.after(badPes.data());
    assert(m.counters().post_pes_bad == 1);

    // HEVC forbidden_zero_bit and temporal_id_plus1 checks.
    Monitor hevc;
    hevc.select(video, 0x24);
    auto goodHevc = ts(video, 0, 0, true);
    pes(goodHevc, 0x26, true);
    hevc.after(goodHevc.data());
    assert(hevc.counters().post_video_nal_headers == 1);
    assert(hevc.counters().post_video_nal_bad == 0);
    auto badHevc = ts(video, 1, 0, true);
    pes(badHevc, 0xA6, true); // forbidden bit set.
    hevc.after(badHevc.data());
    assert(hevc.counters().post_video_nal_bad >= 1);

    m.onEcmResult(false, 100);
    m.onEcmResult(true, 145);
    m.onEcmResult(true, 601);
    assert(m.counters().ecm_not_found == 1);
    assert(m.counters().cw_responses == 2);
    assert(m.counters().ecm_latency_slow == 1);
    assert(m.counters().ecm_latency_max_ms == 601);

    assert(differentService.counters().in_packets == 0);
    assert(differentService.counters().post_video_nal_bad == 0);
    std::cout << "PASS: per-service input TS CC, TEI, AFC, parity, post-CSA PES/NAL and ECM diagnostics\\n";
}
