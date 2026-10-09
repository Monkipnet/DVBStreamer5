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

// V168 deliberately measures the media rate over a long PCR window. Individual
// 20/40 ms VBR PCR intervals are allowed to be above or below the configured
// transport bitrate; the reservoir absorbs those short peaks.
constexpr std::uint64_t kMediaRateWindowTicks = kPcrClockHz * 2ULL;
constexpr auto kArrivalRateWindow = std::chrono::seconds(2);
constexpr auto kControllerInterval = std::chrono::milliseconds(250);
constexpr std::uint64_t kReservoirTargetMs = 2000ULL;
constexpr long double kMaximumReservoirCorrection = 0.08L; // +/-8%
constexpr std::uint64_t kControllerStepPermille = 10ULL;   // 1% per update

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

    // The WISI remultiplexer owns the NULL budget. Source NULL packets carry no
    // useful media and are regenerated at the exact configured transport rate.
    if (isNullPacket(packet)) return true;
    if (!canEnqueue(packet)) return false;

    const auto now = std::chrono::steady_clock::now();
    queuedPackets_.push_back(packet);
    ++totalUsefulPackets_;
    observeArrival(now);
    observeSourcePcr(packet);

    if (!started_) {
        started_ = true;
        nextDeadline_ = now;
        lastControllerUpdate_ = now;
    }
    return true;
}

bool CbrTsPacer::nextDatagram(
    std::chrono::steady_clock::time_point now,
    CbrDatagram& datagram) {
    if (!started_ || now < nextDeadline_) return false;

    // V170: keep the absolute transport phase, but recover scheduler lateness
    // very slowly. V168 could shorten the next inter-datagram gap by the full
    // wakeup error (visible on WISI as 4.0 -> 4.3 Mbit/s). V169 rebased after
    // every tiny late wakeup, making the physical clock systematically slow.
    //
    // The first datagram after the intentional 1.2 s startup reservoir gets a
    // fresh anchor. Afterwards catch-up is capped at 0.25% of one datagram
    // period, so a 4.000 Mbit/s target can only rise to about 4.010 Mbit/s while
    // phase error is being repaid.
    const std::uint64_t periodNs =
        (kDatagramBits * kNanosecondsPerSecond) / targetBitrate_;
    if (outputSlotCounter_ == 0) {
        nextDeadline_ = now;
        pacingRemainder_ = 0;
    } else if (now > nextDeadline_) {
        const auto lateNsSigned = std::chrono::duration_cast<std::chrono::nanoseconds>(
            now - nextDeadline_).count();
        const std::uint64_t lateNs = lateNsSigned > 0
            ? static_cast<std::uint64_t>(lateNsSigned)
            : 0ULL;
        const std::uint64_t maximumPhaseCorrectionNs =
            (std::max<std::uint64_t>)(1ULL, periodNs / 400ULL); // 0.25%
        if (lateNs > maximumPhaseCorrectionNs) {
            nextDeadline_ = now - std::chrono::nanoseconds(maximumPhaseCorrectionNs);
        }
    }

    updateUsefulPace(now);
    std::uint64_t pace = usefulPaceBitrate_;
    if (pace == 0) {
        const std::uint64_t measured = sourcePayloadBitrate_ > 0
            ? sourcePayloadBitrate_
            : arrivalPayloadBitrate_;
        pace = measured > 0
            ? (std::min)(measured, targetBitrate_)
            : (targetBitrate_ * 9ULL) / 10ULL;
    }
    pace = (std::min)(pace, targetBitrate_);

    lastDatagramUsefulPackets_ = 0;
    for (Packet& packet : datagram) {
        bool emitUseful = false;
        usefulToken_ += pace;
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
            // Do not accumulate an unlimited token debt while the network source
            // is temporarily empty; otherwise recovery would create a full-rate
            // useful-packet burst.
            if (queuedPackets_.empty() && usefulToken_ >= targetBitrate_) {
                usefulToken_ = targetBitrate_ - 1ULL;
            }
            makeNullPacket(packet);
        }

        ++outputSlotCounter_;
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

std::size_t CbrTsPacer::lastDatagramUsefulPackets() const noexcept {
    return lastDatagramUsefulPackets_;
}

std::size_t CbrTsPacer::readySegments() const noexcept {
    return queuedPackets_.empty() ? 0U : 1U;
}

std::uint64_t CbrTsPacer::insufficientTargetSegments() const noexcept {
    return sourceOverTargetWindows_;
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
            const std::uint64_t candidate =
                static_cast<std::uint64_t>(measured);
            if (arrivalPayloadBitrate_ == 0) {
                arrivalPayloadBitrate_ = candidate;
            } else {
                // Delivery callbacks can be bursty, so this is only a slow
                // fallback for malformed/no-PCR streams.
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
    if (!havePcrPid_) {
        havePcrPid_ = true;
        pcrPid_ = pid;
    } else if (pid != pcrPid_) {
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
                } else {
                    // Two-second windows already remove local VBR peaks; this
                    // extra EWMA prevents slow source-rate wander from moving the
                    // real/NULL pattern abruptly.
                    sourcePayloadBitrate_ =
                        (sourcePayloadBitrate_ * 3ULL + candidate) / 4ULL;
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
    std::uint64_t desired = static_cast<std::uint64_t>(desiredLd);

    if (usefulPaceBitrate_ == 0) {
        usefulPaceBitrate_ = desired;
        return;
    }

    const std::uint64_t maximumStep =
        (std::max<std::uint64_t>)(1000ULL,
            (base * kControllerStepPermille) / 1000ULL);
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
    if (havePcrPid_ && packetPid(packet) != pcrPid_) return false;

    if (!haveOutputPcrAnchor_ || discontinuity) {
        haveOutputPcrAnchor_ = true;
        outputPcrTicks_ = sourceTicks;
        outputPcrSlot_ = outputSlot;
        if (writePcr(packet, sourceTicks)) {
            ++pcrRewriteCount_;
            return true;
        }
        return false;
    }

    const std::uint64_t deltaSlots = outputSlot >= outputPcrSlot_
        ? outputSlot - outputPcrSlot_
        : 0ULL;
    const unsigned __int128 numerator =
        static_cast<unsigned __int128>(deltaSlots) *
        static_cast<unsigned __int128>(kPacketBits) *
        static_cast<unsigned __int128>(kPcrClockHz);
    const std::uint64_t deltaTicks =
        static_cast<std::uint64_t>(numerator / targetBitrate_);
    const std::uint64_t rewritten =
        (outputPcrTicks_ + deltaTicks) % kPcrWrapTicks;
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
