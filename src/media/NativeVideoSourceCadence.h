#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace dvbstreamer5::media::codec {

// Checks compressed-picture PTS cadence independently of decoded-frame PTS.
// A verified PAL 25-fps source already matches a 25-fps output encoder:
// dropping its decoded pictures because of decoder reorder/field PTS is wrong.
// True 29.97/30->25 conversion remains handled by the original FPS gate.
//
// Only H.264/25-fps uses this source-verified bypass in the production pipeline.
class NativeVideoSourceCadence {
public:
    static constexpr std::size_t kWindow = 33;
    static constexpr std::uint64_t kMask = (1ULL << 33U) - 1ULL;
    static constexpr std::uint64_t kHalf = 1ULL << 32U;

    struct Status {
        bool matches25 = false;
        bool changed = false;
        double measuredFps = 0.0;
    };

    void reset() noexcept {
        count_ = 0;
        next_ = 0;
        matches25_ = false;
        measuredFps_ = 0.0;
    }

    Status observe(std::uint64_t pts90k, bool hasPts) noexcept {
        const bool previous = matches25_;
        if (!hasPts) {
            reset();
            return {matches25_, previous, measuredFps_};
        }
        ring_[next_] = pts90k & kMask;
        next_ = (next_ + 1) % kWindow;
        if (count_ < kWindow) ++count_;
        if (count_ < kWindow) return {false, previous, 0.0};

        // At full capacity, next_ is the oldest PTS, and the preceding slot
        // is the latest. 33 pictures represent 32 presentation intervals.
        const std::uint64_t first = ring_[next_];
        const std::uint64_t last = ring_[(next_ + kWindow - 1) % kWindow];
        const std::uint64_t span = (last - first) & kMask;
        if (span == 0 || span >= kHalf ||
            span > 10ULL * 90000ULL) {
            reset();
            return {false, previous, 0.0};
        }

        measuredFps_ = 32.0 * 90000.0 / static_cast<double>(span);
        // Leave ample margin for PAL 25 and 24.975-fps jitter, but keep
        // 29.97/30, 50 and 59.94-fps sources on the legacy downsampler.
        matches25_ = std::isfinite(measuredFps_) &&
                     measuredFps_ >= 23.5 && measuredFps_ <= 26.5;
        return {matches25_, matches25_ != previous, measuredFps_};
    }

    bool matches25() const noexcept { return matches25_; }
    double measuredFps() const noexcept { return measuredFps_; }

private:
    std::array<std::uint64_t, kWindow> ring_{};
    std::size_t count_ = 0;
    std::size_t next_ = 0;
    bool matches25_ = false;
    double measuredFps_ = 0.0;
};

} // namespace dvbstreamer5::media::codec
