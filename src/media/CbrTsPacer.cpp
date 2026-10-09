#include "media/CbrTsPacer.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace dvbstreamer5::media::mpegts {
namespace {

constexpr std::uint64_t kNanosecondsPerSecond = 1000000000ULL;
constexpr std::uint64_t kPcrClockHz = 27000000ULL;
constexpr std::uint64_t kPcrWrapTicks = (1ULL << 33U) * 300ULL;
constexpr std::uint64_t kDatagramBits =
    kPacketsPerCbrDatagram * kPacketSize * 8ULL;
constexpr std::uint64_t kPacketBits = kPacketSize * 8ULL;
constexpr auto kArrivalRateWindow = std::chrono::milliseconds(250);

// V10.8.166: do not let one jittery PCR interval change real/null placement.
// A 200 ms aggregate is short enough to fit inside the existing 400 ms startup
// reservoir, but long enough to average several normal 20..40 ms PCR intervals.
constexpr std::uint64_t kPcrFilterWindowTicks = kPcrClockHz / 5ULL;
constexpr std::uint64_t kPcrRateDeadbandPermille = 10ULL; // +/-1.0%
constexpr std::uint64_t kPcrRateFollowDivisor = 4ULL;     // move 25% of real change/window

} // namespace

CbrTsPacer::CbrTsPacer(std::uint64_t targetBitrate)
    : targetBitrate_(std::clamp(
          targetBitrate, kMinimumBitrate, kMaximumBitrate)) {
    if (targetBitrate == 0) {
        throw std::invalid_argument("WISI CBR transport bitrate must be greater than zero");
    }
}

bool CbrTsPacer::canEnqueue(const Packet& packet) const noexcept {
    if (packet[0] != kSyncByte) return false;
    if (isNullPacket(packet)) return true;
    return (queuedPackets_.size() + 1U) * kPacketSize <= kMaximumQueuedBytes;
}

bool CbrTsPacer::enqueue(const Packet& packet) {
    if (packet[0] != kSyncByte) return false;

    // The WISI shaper owns the NULL budget. Source NULLs must not enter the
    // useful-packet queue because they would hide the true payload/PCR rate.
    if (isNullPacket(packet)) return true;
    if (!canEnqueue(packet)) return false;

    const auto now = std::chrono::steady_clock::now();
    observeArrival(now);

    QueuedPacket queued;
    queued.packet = packet;
    queued.sequence = nextSequence_++;
    queuedPackets_.push_back(queued);
    observePcr(packet, queued.sequence);

    if (!started_) {
        started_ = true;
        nextDeadline_ = now;
    }
    return true;
}

bool CbrTsPacer::nextDatagram(
    std::chrono::steady_clock::time_point now,
    CbrDatagram& datagram) {
    if (!started_ || now < nextDeadline_) return false;

    // Never repay scheduler latency with a burst. WISI must see one 1316-byte
    // datagram per transport slot, not a train of overdue datagrams followed by
    // a gap.
    const std::uint64_t periodNs =
        (kDatagramBits * kNanosecondsPerSecond) / targetBitrate_;
    const auto maximumCatchup = std::chrono::nanoseconds(
        (std::max<std::uint64_t>)(periodNs, 1ULL));
    if (now - nextDeadline_ >= maximumCatchup) {
        nextDeadline_ = now;
        pacingRemainder_ = 0;
    }

    // Real TS is spread through the fixed transport slots with a Bresenham/token
    // accumulator. V10.8.166 feeds this with the filtered multi-PCR rate, not the
    // instantaneous rate of one PCR interval. The physical CBR/1316-byte sender
    // clock is unchanged.
    for (Packet& packet : datagram) {
        if (queuedPackets_.empty()) {
            makeNullPacket(packet);
            continue;
        }

        const std::uint64_t usefulRate =
            (std::min)(rateForNextPacket(), targetBitrate_);
        usefulToken_ += usefulRate;
        if (usefulToken_ < targetBitrate_) {
            makeNullPacket(packet);
            continue;
        }

        usefulToken_ -= targetBitrate_;
        packet = queuedPackets_.front().packet;
        queuedPackets_.pop_front();
    }

    advanceDeadline();
    return true;
}

bool CbrTsPacer::started() const noexcept { return started_; }

bool CbrTsPacer::timingLocked() const noexcept { return pcrTimingLocked_; }

std::chrono::steady_clock::time_point CbrTsPacer::nextDeadline() const noexcept {
    return nextDeadline_;
}

std::uint64_t CbrTsPacer::targetBitrate() const noexcept { return targetBitrate_; }

std::uint64_t CbrTsPacer::sourcePayloadBitrate() const noexcept {
    if (sourcePayloadBitrate_ > 0) return sourcePayloadBitrate_;
    return arrivalPayloadBitrate_;
}

std::size_t CbrTsPacer::queuedPackets() const noexcept { return queuedPackets_.size(); }

std::size_t CbrTsPacer::queuedBytes() const noexcept {
    return queuedPackets_.size() * kPacketSize;
}

void CbrTsPacer::observeArrival(
    std::chrono::steady_clock::time_point now) noexcept {
    if (!arrivalWindowStarted_) {
        arrivalWindowStarted_ = true;
        arrivalWindowStart_ = now;
        arrivalPackets_ = 1;
        return;
    }

    ++arrivalPackets_;
    if (now - arrivalWindowStart_ < kArrivalRateWindow) return;

    const auto elapsedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now - arrivalWindowStart_).count();
    if (elapsedNs > 0) {
        const long double measured =
            static_cast<long double>(arrivalPackets_) *
            static_cast<long double>(kPacketBits) *
            static_cast<long double>(kNanosecondsPerSecond) /
            static_cast<long double>(elapsedNs);
        if (measured > 0.0L) {
            const auto bounded = static_cast<std::uint64_t>((std::min)(
                measured, static_cast<long double>(kMaximumBitrate)));
            // Arrival rate is only a fallback for malformed/no-PCR streams.
            // Smooth it heavily because HTTP/HLS delivery itself is bursty.
            if (arrivalPayloadBitrate_ == 0) {
                arrivalPayloadBitrate_ = bounded;
            } else {
                arrivalPayloadBitrate_ =
                    (arrivalPayloadBitrate_ * 7ULL + bounded) / 8ULL;
            }
        }
    }

    arrivalWindowStart_ = now;
    arrivalPackets_ = 0;
}

void CbrTsPacer::observePcr(
    const Packet& packet,
    std::uint64_t sequence) noexcept {
    std::uint64_t pcrTicks = 0;
    bool discontinuity = false;
    if (!readPcr(packet, pcrTicks, discontinuity)) return;

    if (discontinuity || !havePreviousPcr_) {
        previousPcrTicks_ = pcrTicks;
        previousPcrSequence_ = sequence;
        havePreviousPcr_ = true;
        resetPcrFilterWindow();
        return;
    }

    const std::uint64_t deltaTicks = pcrTicks >= previousPcrTicks_
        ? pcrTicks - previousPcrTicks_
        : (kPcrWrapTicks - previousPcrTicks_) + pcrTicks;
    const std::uint64_t intervalPackets =
        sequence > previousPcrSequence_ ? sequence - previousPcrSequence_ : 0;

    // Reject obvious PCR resets/corruption. 1 ms..10 s covers normal DVB/IP
    // PCR cadence while treating HLS discontinuities as a new anchor.
    const bool validDelta =
        deltaTicks >= (kPcrClockHz / 1000ULL) &&
        deltaTicks <= (kPcrClockHz * 10ULL) &&
        intervalPackets > 0;

    if (validDelta) {
        if (!pcrFilterWindowActive_) {
            pcrFilterWindowActive_ = true;
            pcrFilterWindowFirstSequence_ = previousPcrSequence_ + 1ULL;
        }
        pcrFilterWindowTicks_ += deltaTicks;
        pcrFilterWindowPackets_ += intervalPackets;
        ++pcrFilterWindowIntervals_;

        if (pcrFilterWindowTicks_ >= kPcrFilterWindowTicks) {
            const long double measured =
                static_cast<long double>(pcrFilterWindowPackets_) *
                static_cast<long double>(kPacketBits) *
                static_cast<long double>(kPcrClockHz) /
                static_cast<long double>(pcrFilterWindowTicks_);
            if (measured > 0.0L &&
                measured <= static_cast<long double>(kMaximumBitrate)) {
                const std::uint64_t candidateRate =
                    (std::max<std::uint64_t>)(1ULL, static_cast<std::uint64_t>(measured));
                const std::uint64_t filteredRate = filterPcrRate(candidateRate);

                // On the first lock stamp the whole startup queue. Afterwards
                // stamp only this aggregate window. Packets not yet stamped fall
                // back to the same filtered sourcePayloadBitrate_, so an interval
                // cannot suddenly revert to an instantaneous PCR-derived value.
                const std::uint64_t firstSequence = pcrTimingLocked_
                    ? pcrFilterWindowFirstSequence_
                    : 0ULL;
                annotatePcrInterval(firstSequence, sequence, filteredRate);
                sourcePayloadBitrate_ = filteredRate;
                pcrTimingLocked_ = true;
            }
            resetPcrFilterWindow();
        }
    } else {
        // Keep the last good filtered source rate through a discontinuity/bad
        // interval and restart accumulation from this PCR anchor.
        resetPcrFilterWindow();
    }

    previousPcrTicks_ = pcrTicks;
    previousPcrSequence_ = sequence;
}

void CbrTsPacer::resetPcrFilterWindow() noexcept {
    pcrFilterWindowActive_ = false;
    pcrFilterWindowTicks_ = 0;
    pcrFilterWindowPackets_ = 0;
    pcrFilterWindowFirstSequence_ = 0;
    pcrFilterWindowIntervals_ = 0;
}

std::uint64_t CbrTsPacer::filterPcrRate(
    std::uint64_t candidateRate) noexcept {
    candidateRate = std::clamp(
        candidateRate, std::uint64_t{1}, kMaximumBitrate);
    if (sourcePayloadBitrate_ == 0) return candidateRate;

    const std::uint64_t current = sourcePayloadBitrate_;
    const std::uint64_t difference = candidateRate > current
        ? candidateRate - current
        : current - candidateRate;

    // Small PCR jitter is deliberately ignored. This is the key V10.8.166
    // behaviour: +/-1% around the established useful rate does not move the
    // real/null packet pattern at all.
    if (difference * 1000ULL <= current * kPcrRateDeadbandPermille) {
        return current;
    }

    // A genuine source-rate change still has to be followed, but only gradually
    // so one abnormal PCR interval cannot create a visible WISI bitrate pulse.
    std::uint64_t step = difference / kPcrRateFollowDivisor;
    if (step == 0) step = 1;
    return candidateRate > current ? current + step : current - step;
}

void CbrTsPacer::annotatePcrInterval(
    std::uint64_t firstSequence,
    std::uint64_t lastSequence,
    std::uint64_t sourceRate) noexcept {
    for (auto& queued : queuedPackets_) {
        if (queued.sequence > lastSequence) break;
        if (firstSequence == 0 || queued.sequence >= firstSequence) {
            queued.sourceRate = sourceRate;
        }
    }
}

std::uint64_t CbrTsPacer::rateForNextPacket() const noexcept {
    if (queuedPackets_.empty()) return 0;
    const std::uint64_t packetRate = queuedPackets_.front().sourceRate;
    if (packetRate > 0) return packetRate;
    if (sourcePayloadBitrate_ > 0) return sourcePayloadBitrate_;
    if (arrivalPayloadBitrate_ > 0) return arrivalPayloadBitrate_;
    return targetBitrate_;
}

bool CbrTsPacer::isNullPacket(const Packet& packet) noexcept {
    const std::uint16_t pid = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(packet[1] & 0x1fU) << 8) | packet[2]);
    return pid == kNullPid;
}

bool CbrTsPacer::readPcr(
    const Packet& packet,
    std::uint64_t& pcrTicks,
    bool& discontinuity) noexcept {
    pcrTicks = 0;
    discontinuity = false;
    if (packet[0] != kSyncByte) return false;

    const std::uint8_t adaptationControl =
        static_cast<std::uint8_t>((packet[3] >> 4U) & 0x03U);
    if (adaptationControl != 2U && adaptationControl != 3U) return false;

    const std::uint8_t adaptationLength = packet[4];
    if (adaptationLength < 1U || adaptationLength > 183U) return false;
    const std::uint8_t flags = packet[5];
    discontinuity = (flags & 0x80U) != 0;
    if ((flags & 0x10U) == 0 || adaptationLength < 7U) return false;

    const std::uint64_t base =
        (static_cast<std::uint64_t>(packet[6]) << 25U) |
        (static_cast<std::uint64_t>(packet[7]) << 17U) |
        (static_cast<std::uint64_t>(packet[8]) << 9U) |
        (static_cast<std::uint64_t>(packet[9]) << 1U) |
        (static_cast<std::uint64_t>(packet[10]) >> 7U);
    const std::uint64_t extension =
        (static_cast<std::uint64_t>(packet[10] & 0x01U) << 8U) |
        static_cast<std::uint64_t>(packet[11]);
    pcrTicks = base * 300ULL + extension;
    return true;
}

void CbrTsPacer::advanceDeadline() noexcept {
    const std::uint64_t numerator = kDatagramBits * kNanosecondsPerSecond;
    std::uint64_t nanoseconds = numerator / targetBitrate_;
    pacingRemainder_ += numerator % targetBitrate_;
    if (pacingRemainder_ >= targetBitrate_) {
        nanoseconds += pacingRemainder_ / targetBitrate_;
        pacingRemainder_ %= targetBitrate_;
    }
    nextDeadline_ += std::chrono::nanoseconds(nanoseconds);
}

void CbrTsPacer::makeNullPacket(Packet& packet) noexcept {
    packet.fill(0xff);
    packet[0] = kSyncByte;
    packet[1] = 0x1f;
    packet[2] = 0xff;
    packet[3] = static_cast<std::uint8_t>(0x10U | nullContinuityCounter_);
    nullContinuityCounter_ = static_cast<std::uint8_t>(
        (nullContinuityCounter_ + 1U) & 0x0fU);
}

} // namespace dvbstreamer5::media::mpegts
