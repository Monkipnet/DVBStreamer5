#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>

namespace dvbstreamer5::media::mpegts {

// Recover the AU presentation clock only from PES PTS attached to the AU's
// byte-stream start. Never borrow the PTS of a PES that starts halfway through
// the previous AU (which can happen when H.264 NALs cross PES boundaries).
class H264PesPtsRecovery {
public:
    static constexpr std::uint64_t kMask33 = (1ULL << 33U) - 1ULL;
    static constexpr std::int64_t kResyncThreshold90k = 45000; // 500 ms

    struct Recovery {
        std::uint64_t pts90k = 0;
        std::uint64_t dts90k = 0;
        bool hasPts = false;
        bool hasDts = false;
        bool corrected = false;
        std::int64_t drift90k = 0;
    };

    void reset() noexcept { anchors_.clear(); }

    void appendPes(std::size_t offset, std::uint64_t pts90k,
                   std::uint64_t dts90k, bool hasPts, bool hasDts) {
        if (!hasPts) return;
        anchors_.push_back({offset, pts90k & kMask33, dts90k & kMask33, hasDts});
    }

    Recovery completeAu(std::size_t consumedBytes, std::uint64_t predictedPts90k,
                        std::uint64_t predictedDts90k, bool hasPts,
                        bool hasDts) {
        Recovery result{predictedPts90k & kMask33,
                        predictedDts90k & kMask33, hasPts, hasDts, false, 0};
        if (!anchors_.empty() && anchors_.front().offset == 0) {
            const Anchor& source = anchors_.front();
            const auto drift = signedPtsDelta90k(source.pts90k, result.pts90k);
            if (!result.hasPts || drift > kResyncThreshold90k ||
                drift < -kResyncThreshold90k) {
                result.pts90k = source.pts90k;
                result.dts90k = source.dts90k;
                result.hasPts = true;
                result.hasDts = source.hasDts;
                result.corrected = true;
                result.drift90k = drift;
            }
        }
        // A PES with a PTS inside this AU may describe a following AU.
        // Do not transfer that timestamp backwards onto the previous frame.
        while (!anchors_.empty() && anchors_.front().offset < consumedBytes)
            anchors_.pop_front();
        for (auto& anchor : anchors_)
            anchor.offset -= consumedBytes;
        return result;
    }

    static std::int64_t signedPtsDelta90k(std::uint64_t source,
                                          std::uint64_t predicted) noexcept {
        constexpr std::uint64_t half = 1ULL << 32U;
        const auto diff = (source - predicted) & kMask33;
        return diff < half ? static_cast<std::int64_t>(diff)
                           : static_cast<std::int64_t>(diff) -
                                 static_cast<std::int64_t>(kMask33 + 1ULL);
    }

private:
    struct Anchor {
        std::size_t offset;
        std::uint64_t pts90k;
        std::uint64_t dts90k;
        bool hasDts;
    };
    std::deque<Anchor> anchors_;
};

} // namespace dvbstreamer5::media::mpegts
