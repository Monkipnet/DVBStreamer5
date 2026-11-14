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

    void reset() noexcept {
        anchors_.clear();
        sourceClockLocked_ = false;
        carriedInteriorAnchors_ = 0;
    }

    std::uint64_t carriedInteriorAnchors() const noexcept {
        return carriedInteriorAnchors_;
    }

    void appendPes(std::size_t offset, std::uint64_t pts90k,
                   std::uint64_t dts90k, bool hasPts, bool hasDts) {
        if (!hasPts) return;
        const Anchor source{offset, pts90k & kMask33, dts90k & kMask33, hasDts};
        if (offset == 0 && !anchors_.empty() && anchors_.front().offset == 0) {
            // An actual PES at the AU boundary supersedes a carried anchor
            // that originated inside the previous picture.
            anchors_.front() = source;
            return;
        }
        anchors_.push_back(source);
    }

    Recovery completeAu(std::size_t consumedBytes, std::uint64_t predictedPts90k,
                        std::uint64_t predictedDts90k, bool hasPts,
                        bool hasDts) {
        Recovery result{predictedPts90k & kMask33,
                        predictedDts90k & kMask33, hasPts, hasDts, false, 0};
        if (!anchors_.empty() && anchors_.front().offset == 0) {
            const Anchor& source = anchors_.front();
            const auto drift = signedPtsDelta90k(source.pts90k, result.pts90k);
            // Lock to the broadcast PES clock only after confirmed phase
            // drift. Afterwards take each aligned source PTS directly, instead
            // of waiting for repeated 500-ms staircase corrections.
            if (!result.hasPts || drift > kResyncThreshold90k ||
                drift < -kResyncThreshold90k)
                sourceClockLocked_ = true;
            if (sourceClockLocked_ &&
                (!result.hasPts || drift != 0 ||
                 result.hasDts != source.hasDts ||
                 (source.hasDts && result.dts90k != source.dts90k))) {
                result.pts90k = source.pts90k;
                result.dts90k = source.dts90k;
                result.hasPts = true;
                result.hasDts = source.hasDts;
                result.corrected = true;
                result.drift90k = drift;
            }
        }
        // The PES PTS belongs to the first access unit STARTING within
        // that PES. A PES header falling in the middle of the current AU
        // applies to the NEXT AU. V526 dropped those PTS anchors entirely.
        Anchor nextAuAnchor{};
        bool carryInteriorAnchor = false;
        while (!anchors_.empty() && anchors_.front().offset < consumedBytes) {
            if (anchors_.front().offset != 0) {
                nextAuAnchor = anchors_.front();
                carryInteriorAnchor = true;
            }
            anchors_.pop_front();
        }
        for (auto& anchor : anchors_)
            anchor.offset -= consumedBytes;
        if (carryInteriorAnchor &&
            (anchors_.empty() || anchors_.front().offset != 0)) {
            nextAuAnchor.offset = 0;
            anchors_.push_front(nextAuAnchor);
            ++carriedInteriorAnchors_;
        }
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
    bool sourceClockLocked_ = false;
    std::uint64_t carriedInteriorAnchors_ = 0;
};

} // namespace dvbstreamer5::media::mpegts
