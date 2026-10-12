#include "media/CbrTsPacer.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace dvbstreamer5::media::mpegts {
namespace {

constexpr std::uint64_t kNanosecondsPerSecond = 1000000000ULL;
constexpr std::uint64_t kPcrClockHz = 27000000ULL;
constexpr std::uint64_t kPcrWrapTicks = (1ULL << 33U) * 300ULL;
constexpr std::uint64_t kDatagramBits =
    kPacketsPerCbrDatagram * kPacketSize * 8ULL;
constexpr std::uint64_t kPacketBits = kPacketSize * 8ULL;

// Source-rate estimation stays separate from the physical UDP sender clock.
// A half-second PCR window locks quickly enough for network/HLS starts while
// averaging normal encoder PCR cadence jitter.
constexpr std::uint64_t kMediaRateWindowTicks = kPcrClockHz / 2ULL;
constexpr auto kArrivalRateWindow = std::chrono::seconds(1);
constexpr auto kControllerInterval = std::chrono::milliseconds(250);
constexpr std::uint64_t kReservoirTargetMs = 1200ULL;
constexpr long double kMaximumReservoirCorrection = 0.03L; // legacy fixed-CBR only
constexpr std::uint64_t kControllerStepDivisor = 400ULL;   // legacy fixed-CBR only

} // namespace

CbrTsPacer::CbrTsPacer(std::uint64_t targetBitrate)
    : sourcePllMode_(sourcePllEncodedTargetBitrate(targetBitrate)),
      targetBitrate_(std::clamp(
          decodeTargetBitrate(targetBitrate), kMinimumBitrate, kMaximumBitrate)) {
    if (decodeTargetBitrate(targetBitrate) == 0) {
        throw std::invalid_argument("WISI CBR transport bitrate must be greater than zero");
    }
}

bool CbrTsPacer::canEnqueue(const Packet& packet) const noexcept {
    if (packet[0] != kSyncByte) return false;
    if (isNullPacket(packet)) return true;
    return (queuedPackets_.size() + 1U) * kPacketSize <= kMaximumQueuedBytes;
}

bool CbrTsPacer::enqueue(const Packet& packet) {
    return enqueueAt(packet, std::chrono::steady_clock::now());
}

bool CbrTsPacer::enqueueAt(
    const Packet& packet,
    std::chrono::steady_clock::time_point arrivalTime) {
    if (packet[0] != kSyncByte) return false;

    // Source NULL packets are not part of the useful media reservoir. The
    // output remultiplexer regenerates the complete NULL budget on the fixed
    // transport clock.
    if (isNullPacket(packet)) return true;
    if (!canEnqueue(packet)) return false;

    queuedPackets_.push_back(packet);
    ++totalUsefulPackets_;
    observeArrival(arrivalTime);
    observeSourcePcr(packet);

    if (!started_) {
        started_ = true;
        nextDeadline_ = arrivalTime;
        lastControllerUpdate_ = arrivalTime;
    }
    return true;
}

bool CbrTsPacer::emitDatagram(
    std::chrono::steady_clock::time_point now,
    CbrDatagram& datagram) {
    if (!started_) return false;

    updateUsefulPace(now);

    std::uint64_t pace = usefulPaceBitrate_;
    if (pace == 0) {
        const std::uint64_t measured = sourcePayloadBitrate_ > 0
            ? sourcePayloadBitrate_
            : arrivalPayloadBitrate_;
        // Before the first PCR-rate window is available, start conservatively.
        pace = measured > 0
            ? (std::min)(measured, targetBitrate_)
            : (targetBitrate_ * 3ULL) / 4ULL;
    }
    pace = (std::max<std::uint64_t>)(1ULL, (std::min)(pace, targetBitrate_));

    lastDatagramUsefulPackets_ = 0;
    for (Packet& packet : datagram) {
        usefulToken_ += pace;
        bool emitUseful = false;
        if (!queuedPackets_.empty() && usefulToken_ >= targetBitrate_) {
            usefulToken_ -= targetBitrate_;
            emitUseful = true;
        }

        if (emitUseful) {
            packet = queuedPackets_.front();
            queuedPackets_.pop_front();
            restampPcr(packet, outputSlotCounter_);
            ++lastDatagramUsefulPackets_;
        } else {
            // Do not accumulate unlimited useful entitlement while an input is
            // temporarily empty. Recovery must remain smooth rather than turn
            // into a full-rate burst of real packets.
            if (queuedPackets_.empty() && usefulToken_ >= targetBitrate_) {
                usefulToken_ = targetBitrate_ - 1ULL;
            }
            makeNullPacket(packet);
        }
        ++outputSlotCounter_;
    }

    return true;
}

bool CbrTsPacer::nextDatagram(
    std::chrono::steady_clock::time_point now,
    CbrDatagram& datagram) {
    if (!started_ || now < nextDeadline_) return false;

    // Compatibility clock for non-UDP-CBR observers. The real WISI UDP sender
    // owns a separate physical transport clock.
    const std::uint64_t periodNs =
        (kDatagramBits * kNanosecondsPerSecond) / targetBitrate_;
    if (now - nextDeadline_ >=
        std::chrono::nanoseconds((std::max<std::uint64_t>)(periodNs, 1ULL))) {
        nextDeadline_ = now;
        pacingRemainder_ = 0;
    }

    if (!emitDatagram(now, datagram)) return false;
    advanceDeadline();
    return true;
}

void CbrTsPacer::reanchorDeadline(
    std::chrono::steady_clock::time_point now) noexcept {
    nextDeadline_ = now;
    pacingRemainder_ = 0;
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

std::uint64_t CbrTsPacer::usefulPaceBitrate() const noexcept {
    return usefulPaceBitrate_;
}

std::size_t CbrTsPacer::queuedPackets() const noexcept { return queuedPackets_.size(); }

std::size_t CbrTsPacer::queuedBytes() const noexcept {
    return queuedPackets_.size() * kPacketSize;
}

std::uint64_t CbrTsPacer::bufferedMediaMilliseconds() const noexcept {
    const std::uint64_t rate = sourcePayloadBitrate_ > 0
        ? sourcePayloadBitrate_
        : arrivalPayloadBitrate_;
    if (rate == 0) return 0;
    const long double bits =
        static_cast<long double>(queuedPackets_.size()) *
        static_cast<long double>(kPacketBits);
    const long double ms = bits * 1000.0L / static_cast<long double>(rate);
    if (ms <= 0.0L) return 0;
    return static_cast<std::uint64_t>(ms);
}

std::uint64_t CbrTsPacer::sourceOverTargetWindows() const noexcept {
    return sourceOverTargetWindows_;
}

std::uint64_t CbrTsPacer::pcrRewriteCount() const noexcept {
    return pcrRewriteCount_;
}

std::uint64_t CbrTsPacer::pcrPidCount() const noexcept {
    return pcrPidCount_;
}

std::size_t CbrTsPacer::lastDatagramUsefulPackets() const noexcept {
    return lastDatagramUsefulPackets_;
}

std::uint64_t CbrTsPacer::outputSlots() const noexcept {
    return outputSlotCounter_;
}

std::size_t CbrTsPacer::readySegments() const noexcept {
    return queuedPackets_.empty() ? 0U : 1U;
}

std::uint64_t CbrTsPacer::insufficientTargetSegments() const noexcept {
    return sourceOverTargetWindows_;
}

bool CbrTsPacer::sourcePacketIsNull(const Packet& packet) noexcept {
    return isNullPacket(packet);
}

void CbrTsPacer::observeArrival(
    std::chrono::steady_clock::time_point now) noexcept {
    if (!arrivalWindowStarted_) {
        arrivalWindowStarted_ = true;
        arrivalWindowStart_ = now;
        arrivalWindowPackets_ = 1;
        return;
    }

    ++arrivalWindowPackets_;
    if (now - arrivalWindowStart_ < kArrivalRateWindow) return;

    const auto elapsedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now - arrivalWindowStart_).count();
    if (elapsedNs > 0) {
        const long double measured =
            static_cast<long double>(arrivalWindowPackets_) *
            static_cast<long double>(kPacketBits) *
            static_cast<long double>(kNanosecondsPerSecond) /
            static_cast<long double>(elapsedNs);
        if (measured > 0.0L &&
            measured <= static_cast<long double>(kMaximumBitrate)) {
            const std::uint64_t candidate = static_cast<std::uint64_t>(measured);
            if (arrivalPayloadBitrate_ == 0) {
                arrivalPayloadBitrate_ = candidate;
            } else {
                arrivalPayloadBitrate_ =
                    (arrivalPayloadBitrate_ * 7ULL + candidate) / 8ULL;
            }
        }
    }

    arrivalWindowStart_ = now;
    arrivalWindowPackets_ = 0;
}

void CbrTsPacer::observeSourcePcr(const Packet& packet) noexcept {
    std::uint64_t pcrTicks = 0;
    bool discontinuity = false;
    if (!readPcr(packet, pcrTicks, discontinuity)) return;

    const std::uint16_t pid = packetPid(packet);
    if (!haveReferencePcrPid_) {
        haveReferencePcrPid_ = true;
        referencePcrPid_ = pid;
    } else if (pid != referencePcrPid_) {
        return;
    }

    if (!havePreviousSourcePcr_ || discontinuity) {
        previousSourcePcrTicks_ = pcrTicks;
        previousSourcePcrPacketCount_ = totalUsefulPackets_;
        havePreviousSourcePcr_ = true;
        mediaWindowTicks_ = 0;
        mediaWindowPackets_ = 0;
        return;
    }

    const std::uint64_t deltaTicks = pcrTicks >= previousSourcePcrTicks_
        ? pcrTicks - previousSourcePcrTicks_
        : (kPcrWrapTicks - previousSourcePcrTicks_) + pcrTicks;
    const std::uint64_t intervalPackets =
        totalUsefulPackets_ >= previousSourcePcrPacketCount_
            ? totalUsefulPackets_ - previousSourcePcrPacketCount_
            : 0ULL;

    const bool valid =
        intervalPackets > 0 &&
        deltaTicks >= (kPcrClockHz / 1000ULL) &&
        deltaTicks <= (kPcrClockHz * 10ULL);

    if (valid) {
        mediaWindowTicks_ += deltaTicks;
        mediaWindowPackets_ += intervalPackets;
        if (mediaWindowTicks_ >= kMediaRateWindowTicks) {
            const long double measured =
                static_cast<long double>(mediaWindowPackets_) *
                static_cast<long double>(kPacketBits) *
                static_cast<long double>(kPcrClockHz) /
                static_cast<long double>(mediaWindowTicks_);
            if (measured > 0.0L &&
                measured <= static_cast<long double>(kMaximumBitrate)) {
                const std::uint64_t candidate =
                    (std::max<std::uint64_t>)(1ULL,
                        static_cast<std::uint64_t>(measured));
                if (candidate > targetBitrate_) {
                    ++sourceOverTargetWindows_;
                }
                if (sourcePayloadBitrate_ == 0) {
                    sourcePayloadBitrate_ = candidate;
                } else if (sourcePllMode_) {
                    // V175 source-timeline mode: the PCR-derived useful rate is
                    // the media clock. Keep the estimator responsive enough to
                    // follow real VBR changes, but do not let queue depth alter
                    // media speed. A 3/4 EWMA removes single-window packet-count
                    // noise without introducing the very slow V174 phase chase.
                    sourcePayloadBitrate_ =
                        (sourcePayloadBitrate_ * 3ULL + candidate) / 4ULL;
                } else {
                    // Legacy fixed-CBR mode keeps the V172/V173 estimator.
                    sourcePayloadBitrate_ =
                        (sourcePayloadBitrate_ * 7ULL + candidate) / 8ULL;
                }
                pcrTimingLocked_ = true;
            }
            mediaWindowTicks_ = 0;
            mediaWindowPackets_ = 0;
        }
    } else {
        mediaWindowTicks_ = 0;
        mediaWindowPackets_ = 0;
    }

    previousSourcePcrTicks_ = pcrTicks;
    previousSourcePcrPacketCount_ = totalUsefulPackets_;
}

void CbrTsPacer::updateUsefulPace(
    std::chrono::steady_clock::time_point now) noexcept {
    if (lastControllerUpdate_ != std::chrono::steady_clock::time_point{} &&
        now - lastControllerUpdate_ < kControllerInterval) {
        return;
    }
    lastControllerUpdate_ = now;

    std::uint64_t base = sourcePayloadBitrate_ > 0
        ? sourcePayloadBitrate_
        : arrivalPayloadBitrate_;
    if (base == 0) return;
    base = (std::min)(base, targetBitrate_);

    if (sourcePllMode_ && sourcePayloadBitrate_ > 0) {
        // V175: source-PCR timeline mode. This is the key behavioural change.
        // The useful media rate is derived only from source PCR elapsed time and
        // useful-packet count. Queue fill level must never accelerate or slow the
        // media clock; the fixed physical CBR sender simply fills all remaining
        // transport slots with NULL packets.
        usefulPaceBitrate_ = base;
        return;
    }

    // Legacy fixed-CBR path retained byte-for-byte in behaviour for streams that
    // do not opt into source-timeline mode.
    const long double targetPacketsExact =
        static_cast<long double>(base) *
        static_cast<long double>(kReservoirTargetMs) /
        (static_cast<long double>(kPacketBits) * 1000.0L);
    const std::uint64_t targetPackets =
        (std::max<std::uint64_t>)(64ULL,
            static_cast<std::uint64_t>(targetPacketsExact));

    const long double error =
        static_cast<long double>(queuedPackets_.size()) -
        static_cast<long double>(targetPackets);
    long double correction = error /
        static_cast<long double>(targetPackets) *
        kMaximumReservoirCorrection;
    correction = (std::max)(-kMaximumReservoirCorrection,
        (std::min)(kMaximumReservoirCorrection, correction));

    long double desiredLd =
        static_cast<long double>(base) * (1.0L + correction);
    desiredLd = (std::max)(1.0L,
        (std::min)(desiredLd, static_cast<long double>(targetBitrate_)));
    const std::uint64_t desired = static_cast<std::uint64_t>(desiredLd);

    if (usefulPaceBitrate_ == 0) {
        usefulPaceBitrate_ = desired;
        return;
    }

    const std::uint64_t maximumStep =
        (std::max<std::uint64_t>)(500ULL, base / kControllerStepDivisor);
    if (desired > usefulPaceBitrate_) {
        usefulPaceBitrate_ +=
            (std::min)(maximumStep, desired - usefulPaceBitrate_);
    } else if (desired < usefulPaceBitrate_) {
        usefulPaceBitrate_ -=
            (std::min)(maximumStep, usefulPaceBitrate_ - desired);
    }
    usefulPaceBitrate_ = (std::min)(usefulPaceBitrate_, targetBitrate_);
}

bool CbrTsPacer::restampPcr(
    Packet& packet,
    std::uint64_t outputSlot) noexcept {
    std::uint64_t sourceTicks = 0;
    bool discontinuity = false;
    if (!readPcr(packet, sourceTicks, discontinuity)) return false;

    const std::uint16_t pid = packetPid(packet);
    if (pid >= outputPcrStates_.size()) return false;
    OutputPcrState& state = outputPcrStates_[pid];

    if (!state.anchored || discontinuity) {
        if (!state.anchored) ++pcrPidCount_;
        state.anchored = true;
        state.sourceAnchorTicks = sourceTicks;
        state.outputAnchorSlot = outputSlot;
        state.previousSourceTicks = sourceTicks;
        state.sourceElapsedTicks = 0;
        state.filteredPhaseTicks = 0;
        state.previousOutputDeltaTicks = 0;
        ++pcrRewriteCount_;
        return true;
    }

    if (sourcePllMode_) {
        // V175 source-PCR timeline mode deliberately preserves every provider
        // PCR value. PTS/DTS are already preserved, so all media timestamps stay
        // in one source clock domain. Correct physical arrival timing is achieved
        // by pacing useful packets from the source-PCR-derived useful rate above;
        // PCR is no longer forced onto the unrelated output-slot phase.
        state.previousSourceTicks = sourceTicks;
        ++pcrRewriteCount_;
        return true;
    }

    const std::uint64_t deltaSlots = outputSlot >= state.outputAnchorSlot
        ? outputSlot - state.outputAnchorSlot
        : 0ULL;
    const unsigned __int128 numerator =
        static_cast<unsigned __int128>(deltaSlots) *
        static_cast<unsigned __int128>(kPacketBits) *
        static_cast<unsigned __int128>(kPcrClockHz);
    const std::uint64_t nominalDeltaTicks =
        static_cast<std::uint64_t>(numerator / targetBitrate_);

    const std::uint64_t rewritten =
        (state.sourceAnchorTicks + nominalDeltaTicks) % kPcrWrapTicks;
    if (!writePcr(packet, rewritten)) return false;
    ++pcrRewriteCount_;
    return true;
}

bool CbrTsPacer::isNullPacket(const Packet& packet) noexcept {
    return packetPid(packet) == kNullPid;
}

std::uint16_t CbrTsPacer::packetPid(const Packet& packet) noexcept {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(packet[1] & 0x1fU) << 8U) | packet[2]);
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

bool CbrTsPacer::writePcr(Packet& packet, std::uint64_t pcrTicks) noexcept {
    if (packet[0] != kSyncByte) return false;
    const std::uint8_t adaptationControl =
        static_cast<std::uint8_t>((packet[3] >> 4U) & 0x03U);
    if (adaptationControl != 2U && adaptationControl != 3U) return false;
    if (packet[4] < 7U || packet[4] > 183U) return false;
    if ((packet[5] & 0x10U) == 0) return false;

    pcrTicks %= kPcrWrapTicks;
    const std::uint64_t base = pcrTicks / 300ULL;
    const std::uint64_t extension = pcrTicks % 300ULL;
    packet[6] = static_cast<std::uint8_t>(base >> 25U);
    packet[7] = static_cast<std::uint8_t>(base >> 17U);
    packet[8] = static_cast<std::uint8_t>(base >> 9U);
    packet[9] = static_cast<std::uint8_t>(base >> 1U);
    packet[10] = static_cast<std::uint8_t>(
        ((base & 0x01ULL) << 7U) | 0x7eU | ((extension >> 8U) & 0x01ULL));
    packet[11] = static_cast<std::uint8_t>(extension & 0xffULL);
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
