#pragma once

#include <cstdint>

namespace dvbstreamer5::media::codec {

// Stable display-order timestamps for field-coded PAL H.264 decoded by
// FFmpeg. AVFrame::best_effort_timestamp may jitter/reorder around the
// expected 25-fps cadence even while decoded pictures themselves are
// sequential, causing the generic output FPS gate to drop ~1 in 5 frames.
//
// Lock only after 25 consecutive *output pictures* prove that the source
// clock is approximately 25 fps. This must NOT rewrite 29.97/30/50 fps
// streams, progressive sources or any non-FFmpeg decoder.
class FfmpegPalPtsClock {
public:
    static constexpr std::uint64_t kMask = (1ULL << 33U) - 1ULL;
    static constexpr std::uint64_t kHalf = 1ULL << 32U;
    static constexpr std::uint64_t kFrameTicks = 3600ULL;

    struct Result {
        std::uint64_t pts90k = 0;
        bool corrected = false;
        bool locked = false;
        bool discontinuity = false;
        std::int64_t sourceDelta90k = 0;
    };

    void reset() noexcept {
        started_ = false;
        locked_ = false;
        anchor_ = 0;
        frameNumber_ = 0;
    }

    Result apply(std::uint64_t sourcePts90k) noexcept {
        sourcePts90k &= kMask;
        if (!started_) {
            started_ = true;
            anchor_ = sourcePts90k;
            frameNumber_ = 1;
            return {sourcePts90k, false, false, false, 0};
        }

        std::uint64_t expected =
            (anchor_ + frameNumber_ * kFrameTicks) & kMask;
        const std::int64_t delta = signedDelta(sourcePts90k, expected);

        if (locked_ && (delta > 90000LL || delta < -90000LL)) {
            // Real PTS discontinuity: re-anchor instead of dragging the
            // former timeline indefinitely into an unrelated segment.
            reset();
            auto result = apply(sourcePts90k);
            result.discontinuity = true;
            return result;
        }

        ++frameNumber_;
        if (!locked_ && frameNumber_ >= 25) {
            // 25 consecutive display frames span 960 ms at PAL cadence.
            // At 30 fps the source runs >120 ms ahead; keep normal FPS
            // downsampling instead of locking this clock.
            if (delta >= -10800LL && delta <= 10800LL) {
                locked_ = true;
                // The first frame of a live GOP can carry a 1-3 field/frame
                // PTS bias. Anchoring the whole outgoing video to that first
                // timestamp manufactures a permanent A/V lead (120 ms in
                // the channel-8 trace). Lock to the current *source* PTS,
                // then advance in exact display-order 25-fps steps.
                const std::uint64_t index = frameNumber_ - 1U;
                anchor_ = (sourcePts90k -
                    ((index * kFrameTicks) & kMask)) & kMask;
                expected = sourcePts90k;
            } else if (frameNumber_ >= 100) {
                // Re-evaluate at a fresh anchor after a slow/fast source.
                anchor_ = sourcePts90k;
                frameNumber_ = 1;
            }
        }
        if (!locked_)
            return {sourcePts90k, false, false, false, delta};

        return {expected, true, frameNumber_ == 25, false, delta};
    }

private:
    static std::int64_t signedDelta(std::uint64_t a,
                                    std::uint64_t b) noexcept {
        const auto difference = (a - b) & kMask;
        return difference < kHalf
            ? static_cast<std::int64_t>(difference)
            : static_cast<std::int64_t>(difference) -
                static_cast<std::int64_t>(kMask + 1ULL);
    }

    bool started_ = false;
    bool locked_ = false;
    std::uint64_t anchor_ = 0;
    std::uint64_t frameNumber_ = 0;
};

} // namespace dvbstreamer5::media::codec
