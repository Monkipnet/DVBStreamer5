#include "media/CbrTsPacer.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>

// Adaptive MPEG-TS CBR sender clock. It still emits seven-packet datagrams on a
// monotonic deadline and fills unused capacity with NULL packets. The configured
// bitrate is a hard floor: adaptive control may only raise the effective CBR when
// real non-null payload approaches/exceeds that floor; it never lowers CBR.
// PCR generation/rewriting remains the mux layer's responsibility.

namespace dvbstreamer5::media::mpegts {
namespace {

constexpr std::uint64_t kNanosecondsPerSecond = 1000000000ULL;
constexpr std::uint64_t kDatagramBits =
    kPacketsPerCbrDatagram * kPacketSize * 8ULL;
constexpr auto kAutoTuneWindow = std::chrono::seconds(4);
constexpr auto kAutoTuneCooldown = std::chrono::seconds(8);
constexpr std::uint64_t kMinimumAdjustment = 100000ULL;

} // namespace

CbrTsPacer::CbrTsPacer(std::uint64_t targetBitrate)
    : configuredTargetBitrate_(std::clamp(
          targetBitrate, kMinimumBitrate, kMaximumBitrate)),
      targetBitrate_(configuredTargetBitrate_) {
    if (targetBitrate == 0) {
        throw std::invalid_argument("CBR transport bitrate must be greater than zero");
    }
}

bool CbrTsPacer::enqueue(const Packet& packet) {
    if (packet[0] != kSyncByte) return false;

    // The CBR shaper owns the NULL-packet budget. Source NULL packets carry no
    // service payload and would otherwise make a heavily padded source look busy
    // and consume queue space. Drop them here; makeNullPacket() recreates exactly
    // the amount needed by the effective output rate.
    if (isNullPacket(packet)) return true;

    observePayloadPacket(std::chrono::steady_clock::now());

    if ((queuedPackets_.size() + 1) * kPacketSize > kMaximumQueuedBytes) {
        return false;
    }
    queuedPackets_.push_back(packet);
    if (!started_) {
        started_ = true;
        nextDeadline_ = std::chrono::steady_clock::now();
    }
    return true;
}

bool CbrTsPacer::nextDatagram(
    std::chrono::steady_clock::time_point now,
    CbrDatagram& datagram) {
    if (!started_ || now < nextDeadline_) {
        return false;
    }

    // V10.8.157: never catch up more than one datagram period. The old fixed
    // 250 ms window allowed several overdue 1316-byte UDP datagrams to be sent
    // back-to-back after ordinary scheduler jitter. Strict receivers such as
    // WISI then measured short 10-20+ Mbit/s bursts followed by gaps even when
    // the long-term average matched the configured CBR.
    //
    // Keep sub-period timing error so the monotonic clock does not drift, but
    // once the sender is at least one whole datagram late, re-anchor to now.
    // advanceDeadline() then puts the next datagram in the future, preventing
    // immediate catch-up bursts.
    const std::uint64_t periodNs =
        (kDatagramBits * kNanosecondsPerSecond) / targetBitrate_;
    const auto maximumCatchup = std::chrono::nanoseconds(
        (std::max<std::uint64_t>)(periodNs, 1ULL));
    if (now - nextDeadline_ >= maximumCatchup) {
        nextDeadline_ = now;
        pacingRemainder_ = 0;
    }

    for (Packet& packet : datagram) {
        if (queuedPackets_.empty()) {
            makeNullPacket(packet);
        } else {
            packet = queuedPackets_.front();
            queuedPackets_.pop_front();
        }
    }
    advanceDeadline();
    return true;
}

bool CbrTsPacer::started() const noexcept {
    return started_;
}

std::chrono::steady_clock::time_point CbrTsPacer::nextDeadline() const noexcept {
    return nextDeadline_;
}

std::uint64_t CbrTsPacer::targetBitrate() const noexcept {
    return targetBitrate_;
}

std::size_t CbrTsPacer::queuedPackets() const noexcept {
    return queuedPackets_.size();
}

void CbrTsPacer::observePayloadPacket(
    std::chrono::steady_clock::time_point now) noexcept {
    if (!payloadRateWindowStarted_) {
        payloadRateWindowStarted_ = true;
        payloadRateWindowStart_ = now;
        payloadPacketsInWindow_ = 1;
        return;
    }

    ++payloadPacketsInWindow_;
    if (now - payloadRateWindowStart_ >= kAutoTuneWindow) {
        finishPayloadRateWindow(now);
    }
}

void CbrTsPacer::finishPayloadRateWindow(
    std::chrono::steady_clock::time_point now) noexcept {
    const auto elapsedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now - payloadRateWindowStart_).count();
    if (elapsedNs <= 0) return;

    const std::uint64_t measured = static_cast<std::uint64_t>(
        (static_cast<long double>(payloadPacketsInWindow_) *
         static_cast<long double>(kPacketSize * 8ULL) *
         static_cast<long double>(kNanosecondsPerSecond)) /
        static_cast<long double>(elapsedNs));

    recentPayloadRates_[recentPayloadRateIndex_] = measured;
    recentPayloadRateIndex_ =
        (recentPayloadRateIndex_ + 1U) % recentPayloadRates_.size();
    recentPayloadRateCount_ =
        (std::min)(recentPayloadRateCount_ + 1U, recentPayloadRates_.size());

    std::uint64_t recentPeak = 0;
    for (std::size_t i = 0; i < recentPayloadRateCount_; ++i) {
        recentPeak = (std::max)(recentPeak, recentPayloadRates_[i]);
    }

    // Leave room for short video peaks, audio/PSI/PES overhead and scheduler
    // jitter. 12.5% or 300 kbit/s (whichever is larger) keeps the effective CBR
    // safely above measured payload while avoiding queue growth.
    const std::uint64_t headroom = std::max<std::uint64_t>(
        recentPeak / 8U, 300000U);
    const std::uint64_t desired = std::clamp(
        roundUp100K(recentPeak + headroom),
        configuredTargetBitrate_, kMaximumBitrate);

    const bool cooldownFinished = !autoTuneApplied_ ||
        now - lastAutoTune_ >= kAutoTuneCooldown;
    if (cooldownFinished && recentPeak > 0) {
        // V10.8.148: adaptive CBR is raise-only. The configured bitrate is a
        // hard floor for the lifetime of the pacer. This deliberately prevents
        // down/up/down oscillation that can make strict receivers such as WISI
        // temporarily lose service lock when the transport rate keeps changing.
        if (recentPeak * 100ULL >= targetBitrate_ * 92ULL &&
            desired >= targetBitrate_ + kMinimumAdjustment) {
            applyTargetBitrate(desired, recentPeak, "up");
        }
    }

    payloadRateWindowStart_ = now;
    payloadPacketsInWindow_ = 0;
}

void CbrTsPacer::applyTargetBitrate(
    std::uint64_t targetBitrate,
    std::uint64_t measuredPayloadBitrate,
    const char* direction) noexcept {
    targetBitrate = std::clamp(
        targetBitrate, configuredTargetBitrate_, kMaximumBitrate);
    if (targetBitrate <= targetBitrate_) return;

    const std::uint64_t oldTarget = targetBitrate_;
    targetBitrate_ = targetBitrate;
    pacingRemainder_ = 0;
    lastAutoTune_ = std::chrono::steady_clock::now();
    autoTuneApplied_ = true;

    std::cerr << "CBR AUTO TUNE direction=" << direction
              << " configured_kbps=" << (configuredTargetBitrate_ / 1000ULL)
              << " measured_payload_kbps=" << (measuredPayloadBitrate / 1000ULL)
              << " old_kbps=" << (oldTarget / 1000ULL)
              << " new_kbps=" << (targetBitrate_ / 1000ULL)
              << std::endl;
}

bool CbrTsPacer::isNullPacket(const Packet& packet) noexcept {
    const std::uint16_t pid = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(packet[1] & 0x1fU) << 8) | packet[2]);
    return pid == kNullPid;
}

std::uint64_t CbrTsPacer::roundUp100K(std::uint64_t bitrate) noexcept {
    constexpr std::uint64_t quantum = 100000ULL;
    if (bitrate > kMaximumBitrate - (quantum - 1ULL)) {
        return kMaximumBitrate;
    }
    return ((bitrate + quantum - 1ULL) / quantum) * quantum;
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
