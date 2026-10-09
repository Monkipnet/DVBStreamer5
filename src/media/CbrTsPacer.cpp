#include "media/CbrTsPacer.h"

#include <algorithm>
#include <stdexcept>

namespace dvbstreamer5::media::mpegts {
namespace {

constexpr std::uint64_t kNanosecondsPerSecond = 1000000000ULL;
constexpr std::uint64_t kPcrClockHz = 27000000ULL;
constexpr std::uint64_t kPcrWrapTicks = (1ULL << 33U) * 300ULL;
constexpr std::uint64_t kDatagramBits =
    kPacketsPerCbrDatagram * kPacketSize * 8ULL;
constexpr std::uint64_t kPacketBits = kPacketSize * 8ULL;

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

    // WISI CBR owns the NULL budget. Source NULL packets are removed before the
    // PCR interval is built; the exact target-CBR NULL budget is generated on
    // output instead.
    if (isNullPacket(packet)) return true;
    if (!canEnqueue(packet)) return false;

    const auto now = std::chrono::steady_clock::now();
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

    // Never repay scheduler latency with a burst. One physical 1316-byte UDP
    // datagram is emitted per transport deadline; after a late wakeup the clock
    // is simply rebased to now.
    const std::uint64_t periodNs =
        (kDatagramBits * kNanosecondsPerSecond) / targetBitrate_;
    const auto maximumCatchup = std::chrono::nanoseconds(
        (std::max<std::uint64_t>)(periodNs, 1ULL));
    if (now - nextDeadline_ >= maximumCatchup) {
        nextDeadline_ = now;
        pacingRemainder_ = 0;
    }

    for (Packet& packet : datagram) {
        if (!activeSegment_ && !activateNextSegment()) {
            // The physical transport remains CBR while waiting for the closing
            // PCR of the next useful interval. A 1.2 s upstream reservoir in the
            // WISI worker normally keeps several complete intervals ready.
            makeNullPacket(packet);
            continue;
        }

        bool emitReal = false;
        if (activeForceFirstReal_) {
            // A source-PCR packet starts every exact PCR segment and must occupy
            // the first target slot. This is what makes PCR-to-PCR packet spacing
            // deterministic at the configured transport bitrate.
            activeForceFirstReal_ = false;
            emitReal = true;
        } else if (activeSpreadSlotsTotal_ != 0 && activeSpreadRealTotal_ != 0) {
            activeToken_ += activeSpreadRealTotal_;
            if (activeToken_ >= activeSpreadSlotsTotal_) {
                activeToken_ -= activeSpreadSlotsTotal_;
                emitReal = true;
            }
        }

        if (emitReal) {
            if (!emitRealFromActiveSegment(packet)) {
                makeNullPacket(packet);
            }
        } else {
            makeNullPacket(packet);
        }

        if (activeSlotsRemaining_ > 0) --activeSlotsRemaining_;
        if (activeSlotsRemaining_ == 0) {
            // With slots >= realPackets and the deterministic accumulator,
            // activeRealRemaining_ is expected to be zero. Keep a data-preserving
            // emergency tail only for an internal accounting mismatch.
            if (activeRealRemaining_ != 0) {
                activeSlotsRemaining_ = activeRealRemaining_;
                activeSlotsTotal_ += activeRealRemaining_;
                activeSpreadRealTotal_ = activeRealRemaining_;
                activeSpreadSlotsTotal_ = activeRealRemaining_;
                activeToken_ = 0;
                activeForceFirstReal_ = false;
            } else {
                activeSegment_ = false;
            }
        }
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
    return sourcePayloadBitrate_;
}

std::size_t CbrTsPacer::queuedPackets() const noexcept { return queuedPackets_.size(); }

std::size_t CbrTsPacer::queuedBytes() const noexcept {
    return queuedPackets_.size() * kPacketSize;
}

std::size_t CbrTsPacer::readySegments() const noexcept {
    return readySegments_.size() + (activeSegment_ ? 1U : 0U);
}

std::uint64_t CbrTsPacer::insufficientTargetSegments() const noexcept {
    return insufficientTargetSegments_;
}

void CbrTsPacer::observePcr(
    const Packet& packet,
    std::uint64_t sequence) noexcept {
    std::uint64_t pcrTicks = 0;
    bool discontinuity = false;
    if (!readPcr(packet, pcrTicks, discontinuity)) return;

    const std::uint16_t pid = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(packet[1] & 0x1fU) << 8U) | packet[2]);
    if (!havePcrPid_) {
        pcrPid_ = pid;
        havePcrPid_ = true;
    } else if (pid != pcrPid_) {
        // Do not mix independent PCR domains if an upstream MPTS leaks more than
        // one service through the relay. One WISI output must be scheduled from
        // one PCR clock only.
        return;
    }

    if (!havePreviousPcr_) {
        previousPcrTicks_ = pcrTicks;
        previousPcrSequence_ = sequence;
        havePreviousPcr_ = true;
        return;
    }

    if (discontinuity) {
        if (pcrTimingLocked_ && sequence > previousPcrSequence_) {
            queueFallbackSegment(previousPcrSequence_, sequence - 1ULL);
        }
        previousPcrTicks_ = pcrTicks;
        previousPcrSequence_ = sequence;
        return;
    }

    const std::uint64_t deltaTicks = pcrTicks >= previousPcrTicks_
        ? pcrTicks - previousPcrTicks_
        : (kPcrWrapTicks - previousPcrTicks_) + pcrTicks;

    const bool validDelta =
        sequence > previousPcrSequence_ &&
        deltaTicks >= (kPcrClockHz / 1000ULL) &&
        deltaTicks <= (kPcrClockHz * 10ULL);

    if (validDelta) {
        if (!pcrTimingLocked_ && !queuedPackets_.empty() &&
            queuedPackets_.front().sequence < previousPcrSequence_) {
            queuePrefixSegment(
                queuedPackets_.front().sequence,
                previousPcrSequence_ - 1ULL);
        }

        queuePcrSegment(
            previousPcrSequence_,
            sequence - 1ULL,
            deltaTicks);
        pcrTimingLocked_ = true;
    } else if (pcrTimingLocked_ && sequence > previousPcrSequence_) {
        // HLS discontinuities and damaged PCR jumps must not strand useful
        // packets forever at the head of the queue. Preserve the last good
        // media rate for just this one incomplete interval, then re-anchor.
        queueFallbackSegment(previousPcrSequence_, sequence - 1ULL);
    }

    previousPcrTicks_ = pcrTicks;
    previousPcrSequence_ = sequence;
}

std::uint64_t CbrTsPacer::countQueuedPackets(
    std::uint64_t firstSequence,
    std::uint64_t lastSequence) const noexcept {
    if (firstSequence == 0 || lastSequence < firstSequence) return 0;
    std::uint64_t count = 0;
    for (const auto& queued : queuedPackets_) {
        if (queued.sequence < firstSequence) continue;
        if (queued.sequence > lastSequence) break;
        ++count;
    }
    return count;
}

void CbrTsPacer::queuePrefixSegment(
    std::uint64_t firstSequence,
    std::uint64_t lastSequence) noexcept {
    const std::uint64_t realPackets =
        countQueuedPackets(firstSequence, lastSequence);
    if (realPackets == 0) return;

    ReadySegment segment;
    segment.firstSequence = firstSequence;
    segment.lastSequence = lastSequence;
    segment.realPackets = realPackets;
    segment.slots = realPackets;
    segment.startsWithPcr = false;
    readySegments_.push_back(segment);
}

void CbrTsPacer::queuePcrSegment(
    std::uint64_t firstSequence,
    std::uint64_t lastSequence,
    std::uint64_t deltaTicks) noexcept {
    const std::uint64_t realPackets =
        countQueuedPackets(firstSequence, lastSequence);
    if (realPackets == 0 || deltaTicks == 0) return;

    constexpr std::uint64_t denominator = kPcrClockHz * kPacketBits;
    const std::uint64_t numerator = targetBitrate_ * deltaTicks;
    std::uint64_t slots = (numerator + denominator / 2ULL) / denominator;
    if (slots == 0) slots = 1;
    if (slots < realPackets) {
        slots = realPackets;
        ++insufficientTargetSegments_;
    }

    ReadySegment segment;
    segment.firstSequence = firstSequence;
    segment.lastSequence = lastSequence;
    segment.realPackets = realPackets;
    segment.slots = slots;
    segment.startsWithPcr = true;
    readySegments_.push_back(segment);

    const long double measured =
        static_cast<long double>(realPackets) *
        static_cast<long double>(kPacketBits) *
        static_cast<long double>(kPcrClockHz) /
        static_cast<long double>(deltaTicks);
    if (measured > 0.0L && measured <= static_cast<long double>(kMaximumBitrate)) {
        sourcePayloadBitrate_ =
            (std::max<std::uint64_t>)(1ULL, static_cast<std::uint64_t>(measured));
    }
}

void CbrTsPacer::queueFallbackSegment(
    std::uint64_t firstSequence,
    std::uint64_t lastSequence) noexcept {
    const std::uint64_t realPackets =
        countQueuedPackets(firstSequence, lastSequence);
    if (realPackets == 0) return;

    std::uint64_t slots = realPackets;
    if (sourcePayloadBitrate_ > 0) {
        const std::uint64_t numerator = realPackets * targetBitrate_;
        slots = (numerator + sourcePayloadBitrate_ - 1ULL) /
            sourcePayloadBitrate_;
        if (slots < realPackets) {
            slots = realPackets;
            ++insufficientTargetSegments_;
        }
    }

    ReadySegment segment;
    segment.firstSequence = firstSequence;
    segment.lastSequence = lastSequence;
    segment.realPackets = realPackets;
    segment.slots = slots;
    segment.startsWithPcr = true;
    readySegments_.push_back(segment);
}

bool CbrTsPacer::activateNextSegment() noexcept {
    if (activeSegment_) return true;
    if (readySegments_.empty()) return false;

    const ReadySegment segment = readySegments_.front();
    readySegments_.pop_front();
    if (segment.slots == 0) return activateNextSegment();

    activeSegment_ = true;
    activeFirstSequence_ = segment.firstSequence;
    activeLastSequence_ = segment.lastSequence;
    activeRealTotal_ = segment.realPackets;
    activeRealRemaining_ = segment.realPackets;
    activeSlotsTotal_ = segment.slots;
    activeSlotsRemaining_ = segment.slots;
    activeForceFirstReal_ = segment.startsWithPcr && segment.realPackets != 0;

    const std::uint64_t forced = activeForceFirstReal_ ? 1ULL : 0ULL;
    activeSpreadRealTotal_ = segment.realPackets >= forced
        ? segment.realPackets - forced
        : 0ULL;
    activeSpreadSlotsTotal_ = segment.slots >= forced
        ? segment.slots - forced
        : 0ULL;
    activeToken_ = 0;
    return true;
}

bool CbrTsPacer::emitRealFromActiveSegment(Packet& packet) noexcept {
    if (!activeSegment_ || activeRealRemaining_ == 0 || queuedPackets_.empty()) {
        return false;
    }

    const auto& queued = queuedPackets_.front();
    if (queued.sequence < activeFirstSequence_ ||
        queued.sequence > activeLastSequence_) {
        return false;
    }

    packet = queued.packet;
    queuedPackets_.pop_front();
    --activeRealRemaining_;
    return true;
}

bool CbrTsPacer::isNullPacket(const Packet& packet) noexcept {
    const std::uint16_t pid = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(packet[1] & 0x1fU) << 8U) | packet[2]);
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
