#pragma once

#include <cstdint>
#include <deque>

namespace dvbstreamer5::media::mpegts {

// Maps useful-packet positions onto the reference PID's PCR timeline. Unlike
// an averaged useful bitrate, the bracketing PCR interval follows VBR changes
// already held in the startup reservoir. No packet bytes or sender deadlines
// are changed here. Missing lookahead uses the last interval, never drops data.
class SourcePcrPacketSchedule {
public:
    void observe(std::uint64_t index, std::uint64_t pcr, bool discontinuity) {
        if (anchors_.empty()) {
            anchors_.push_back({index, 0});
            previousPcr_ = pcr;
            return;
        }
        const auto previous = anchors_.back();
        if (index <= previous.index) return;
        const auto packets = index - previous.index;
        const auto delta = (pcr + kWrap - previousPcr_) % kWrap;
        if (!discontinuity && delta == 0) return; // repeated source PCR
        std::uint64_t elapsed;
        if (!discontinuity && delta > 0 && delta <= kHz * 10ULL) {
            intervalTicks_ = delta;
            intervalPackets_ = packets;
            elapsed = delta;
        } else if (ready()) {
            // An explicit clock reset (or damaged PCR) must not reinterpret
            // packets still queued before the reset. Bridge only this boundary
            // using the last known interval; subsequent PCRs define a new one.
            elapsed = static_cast<std::uint64_t>(
                static_cast<unsigned __int128>(packets) * intervalTicks_ /
                intervalPackets_);
        } else {
            anchors_.clear();
            anchors_.push_back({index, 0});
            previousPcr_ = pcr;
            return;
        }
        anchors_.push_back({index, previous.ticks + elapsed});
        previousPcr_ = pcr;
    }

    bool ready() const noexcept { return intervalPackets_ != 0; }

    bool due(std::uint64_t index, std::uint64_t slot, std::uint64_t bitrate) {
        while (anchors_.size() > 2 && index >= anchors_[1].index)
            anchors_.pop_front();
        const auto& left = anchors_.front();
        auto ticks = intervalTicks_;
        auto packets = intervalPackets_;
        if (anchors_.size() > 1 && index <= anchors_[1].index) {
            ticks = anchors_[1].ticks - left.ticks;
            packets = anchors_[1].index - left.index;
        } else if (anchors_.size() > 1) {
            // Extrapolation past the newest PCR uses its own last interval.
            ticks = anchors_.back().ticks - left.ticks;
            packets = anchors_.back().index - left.index;
        }
        const auto position = static_cast<std::int64_t>(index) -
                              static_cast<std::int64_t>(left.index);
        const auto sourceTicks = static_cast<std::int64_t>(left.ticks) +
            static_cast<std::int64_t>(
                static_cast<__int128>(position) * ticks / packets);
        if (!anchored_) {
            sourceAnchor_ = sourceTicks;
            slotAnchor_ = slot;
            anchored_ = true;
        }
        const auto sourceDelta = sourceTicks - sourceAnchor_;
        if (sourceDelta <= 0) return true;
        return static_cast<unsigned __int128>(sourceDelta) * bitrate <=
            static_cast<unsigned __int128>(slot - slotAnchor_) * 188ULL * 8ULL * kHz;
    }

private:
    struct Anchor { std::uint64_t index, ticks; };
    static constexpr std::uint64_t kHz = 27000000ULL;
    static constexpr std::uint64_t kWrap = (1ULL << 33U) * 300ULL;
    std::deque<Anchor> anchors_;
    std::uint64_t previousPcr_ = 0;
    std::uint64_t intervalTicks_ = 0;
    std::uint64_t intervalPackets_ = 0;
    bool anchored_ = false;
    std::int64_t sourceAnchor_ = 0;
    std::uint64_t slotAnchor_ = 0;
};

} // namespace dvbstreamer5::media::mpegts
