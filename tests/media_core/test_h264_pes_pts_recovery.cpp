#include "media/H264PesPtsRecovery.h"

#ifdef NDEBUG
#undef NDEBUG // Keep the regression assertions active in Release builds.
#endif
#include <cassert>
#include <cstdint>
#include <iostream>

using dvbstreamer5::media::mpegts::H264PesPtsRecovery;

int main() {
    H264PesPtsRecovery pts;
    constexpr std::uint64_t base = 5000000;
    pts.appendPes(0, base, base - 3600, true, true);
    auto a = pts.completeAu(100, base, base - 3600, true, true);
    assert(!a.corrected && a.pts90k == base);

    // Benign codec jitter below 500 ms must not undo timestamp prediction.
    pts.appendPes(0, base + 3600, base, true, true);
    a = pts.completeAu(100, base + 5400, base + 1800, true, true);
    assert(!a.corrected && a.pts90k == base + 5400);

    // Source PES at the following AU boundary belongs to that next AU,
    // not to the unfinished picture currently being emitted.
    pts.appendPes(100, base + 90000, base + 86400, true, true);
    a = pts.completeAu(100, base + 7200, base + 3600, true, true);
    assert(!a.corrected);
    a = pts.completeAu(100, base + 7200, base + 3600, true, true);
    assert(a.corrected && a.pts90k == base + 90000 &&
           a.dts90k == base + 86400 && a.hasDts &&
           a.drift90k == 82800);

    // After lock, small phase errors are corrected continuously instead of
    // accumulating into recurring 500-ms A/V steps.
    pts.appendPes(0, base + 93000, base + 89400, true, true);
    a = pts.completeAu(100, base + 93600, base + 90000, true, true);
    assert(a.corrected && a.pts90k == base + 93000 &&
           a.drift90k == -600);

    // A PES beginning in the middle of the preceding NAL may carry the
    // *next* picture PTS: do not attach it to the preceding picture.
    pts.appendPes(50, base + 150000, base + 146400, true, true);
    a = pts.completeAu(100, base + 93600, base + 90000, true, true);
    assert(!a.corrected);
    pts.appendPes(0, base + 180000, base + 176400, true, true);
    a = pts.completeAu(100, base + 97200, base + 93600, true, true);
    assert(a.corrected && a.pts90k == base + 180000);

    // No PES header timestamp: preserve normal predicted cadence.
    a = pts.completeAu(100, base + 183600, base + 180000, true, true);
    assert(!a.corrected && a.pts90k == base + 183600);

    // Home2 regression: a 25-fps H.264 AU assembler incrementing only
    // 1800 ticks for each real 3600-tick frame used to accumulate a
    // 140-second skew. Verify the new clock stays within 500 ms over
    // 250 frames of simulated erroneous field-duration predictions.
    pts.reset();
    std::uint64_t predicted = base;
    for (int i = 0; i < 250; ++i) {
        const std::uint64_t source = base + 3600ULL * i;
        pts.appendPes(0, source, source, true, true);
        auto frame = pts.completeAu(100, predicted, predicted, true, true);
        const auto delta = H264PesPtsRecovery::signedPtsDelta90k(source, frame.pts90k);
        assert(delta <= 45000 && delta >= -45000);
        predicted = (frame.pts90k + 1800ULL) & H264PesPtsRecovery::kMask33;
    }

    // V527: a PES may start in the middle of an unfinished NAL.
    // Its PTS must be carried to the FIRST following AU, not discarded.
    // Reproduce a real 25-fps source with a broken 1800-tick predictor
    // over 1000 AUs. V526 loses every interior anchor and fails this test.
    pts.reset();
    predicted = base;
    for (int i = 0; i < 1000; ++i) {
        const std::uint64_t source = base + 3600ULL * i;
        pts.appendPes(50, source + 3600ULL, source + 3600ULL, true, true);
        const auto frame = pts.completeAu(100, predicted, predicted, true, true);
        const auto delta = H264PesPtsRecovery::signedPtsDelta90k(
            source, frame.pts90k);
        assert(delta <= H264PesPtsRecovery::kResyncThreshold90k);
        assert(delta >= -H264PesPtsRecovery::kResyncThreshold90k);
        predicted = (frame.pts90k + 1800ULL) & H264PesPtsRecovery::kMask33;
    }
    assert(pts.carriedInteriorAnchors() == 1000);

    // If the next AU has a PES header at its exact byte boundary,
    // it supersedes a timestamp carried from the previous NAL.
    pts.reset();
    pts.appendPes(40, base + 90000, base + 90000, true, true);
    pts.appendPes(100, base + 180000, base + 180000, true, true);
    a = pts.completeAu(100, base, base, true, true);
    assert(!a.corrected);
    a = pts.completeAu(100, base + 1800, base + 1800, true, true);
    assert(a.corrected && a.pts90k == base + 180000);

    // Signed MPEG 33-bit wrap-aware delta: +/-1 s remains +/-90000.
    constexpr std::uint64_t wrap = 1ULL << 33U;
    assert(H264PesPtsRecovery::signedPtsDelta90k(45000, wrap - 45000) == 90000);
    assert(H264PesPtsRecovery::signedPtsDelta90k(wrap - 45000, 45000) == -90000);
    pts.reset();
    pts.appendPes(0, 45000, 45000, true, true);
    a = pts.completeAu(100, wrap - 45000, wrap - 45000, true, true);
    assert(a.corrected && a.pts90k == 45000);

    std::cout << "PASS: H264 PES PTS reanchor, split PES, clock wrap\n";
    return 0;
}
