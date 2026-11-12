#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace dvbstreamer5::media::transcode {

// Decode-order PES pacing with bounded wait. DTS, unlike presentation PTS,
// respects H.264 B-picture reorder. When absent, use nominal decode cadence.
// No compressed frames dropped and no PTS/DTS timestamps rewritten.
class NativeVideoPesPacer {
public:
    using Clock = std::chrono::steady_clock;
    struct Plan {
        Clock::time_point deadline{};
        bool usedDts = false;
        bool resynced = false;
        bool budgetCapped = false;
    };

    explicit NativeVideoPesPacer(
        std::chrono::milliseconds nominalStep = std::chrono::milliseconds(40),
        std::chrono::milliseconds maxWait = std::chrono::milliseconds(200)) noexcept
        : nominalStep_(nominalStep), maxWait_(maxWait) {}

    void reset() noexcept {
        initialized_ = false;
        lastHasDts_ = false;
        lastDts90k_ = 0;
        lastDeadline_ = {};
    }

    Plan plan(Clock::time_point now, bool hasDts, std::uint64_t dts90k) noexcept {
        constexpr std::uint64_t kMask = (1ULL << 33U) - 1ULL;
        Plan result;
        auto step = nominalStep_;
        if (initialized_ && hasDts && lastHasDts_) {
            const auto delta = (dts90k - lastDts90k_) & kMask;
            // 10..100 ms is a plausible single compressed-picture interval.
            // Other nonzero jumps re-anchor instead of flooding the decoder.
            if (delta >= 900 && delta <= 9000) {
                step = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::microseconds((delta * 1000000ULL + 45000ULL) / 90000ULL));
                result.usedDts = true;
            } else if (delta != 0) {
                result.resynced = true;
            }
        }
        if (!initialized_ || result.resynced) {
            result.deadline = now + nominalStep_;
        } else {
            result.deadline = std::max(now, lastDeadline_ + step);
            if (lastDeadline_ + step + maxWait_ < now)
                result.resynced = true;
        }
        if (result.deadline > now + maxWait_) {
            result.deadline = now + maxWait_;
            result.budgetCapped = true;
        }
        initialized_ = true;
        lastDeadline_ = result.deadline;
        lastHasDts_ = hasDts;
        if (hasDts) lastDts90k_ = dts90k & kMask;
        return result;
    }

private:
    std::chrono::milliseconds nominalStep_;
    std::chrono::milliseconds maxWait_;
    bool initialized_ = false;
    bool lastHasDts_ = false;
    std::uint64_t lastDts90k_ = 0;
    Clock::time_point lastDeadline_{};
};

} // namespace dvbstreamer5::media::transcode
