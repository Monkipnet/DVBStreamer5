#include "media/CbrTsPacer.h"

#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>

// Adaptive MPEG-TS CBR sender clock. Standard mode keeps the native DVBStreamer5
// shaper. TvStreammerSat5Network ports the stable continuous SRT/HTTP strategy
// from TVStreammerSAT5 StableUdpOutput: a real jitter reservoir, uniform useful
// packet token pacing, strict 7x188 CBR slots, NULL stuffing and an independent
// continuous 20 ms PCR media clock, matching StableUdpOutput.

namespace dvbstreamer5::media::mpegts {
namespace {

constexpr std::uint64_t kNanosecondsPerSecond = 1000000000ULL;
constexpr std::uint64_t kDatagramBits =
    kPacketsPerCbrDatagram * kPacketSize * 8ULL;
constexpr std::uint64_t kPacketBits = kPacketSize * 8ULL;
constexpr auto kAutoTuneWindow = std::chrono::seconds(4);
constexpr auto kAutoTuneCooldown = std::chrono::seconds(8);
constexpr std::uint64_t kMinimumAdjustment = 100000ULL;

// TVStreammerSAT5 StableUdpOutput continuous-network profile.
constexpr auto kTvSatStartupReservoir = std::chrono::seconds(5);
// SAT5 continuous-network profile (202.22): rate is measured over a
// 15-second arrival window after at least five seconds of evidence.
constexpr auto kTvSatArrivalSampleInterval = std::chrono::milliseconds(500);
constexpr auto kTvSatArrivalRateWindow = std::chrono::seconds(15);
constexpr auto kTvSatArrivalRateMinimum = std::chrono::seconds(5);
// The reservoir PLL intentionally reacts much more slowly than the input
// reader: one update every two seconds, max +/-1% correction and 0.1% step.
constexpr auto kTvSatControllerUpdate = std::chrono::seconds(2);
constexpr auto kTvSatTargetReservoir = std::chrono::milliseconds(2500);
constexpr std::uint64_t kTvSatPllCorrectionPermille = 10ULL;
constexpr std::uint64_t kTvSatPllStepPermille = 1ULL;
constexpr std::uint64_t kTvSatPllFollowDivisor = 64ULL;
constexpr auto kTvSatPeriodicPcrInterval = std::chrono::milliseconds(20);
constexpr std::size_t kTvSatStartupMinimumPcrSamples = 5;
constexpr std::uint64_t kPcrClockHz = 27000000ULL;
constexpr std::uint64_t kPcrTicksModulus = (1ULL << 33) * 300ULL;

std::uint64_t bitrateForPackets(
    std::uint64_t packets,
    std::chrono::nanoseconds elapsed) noexcept {
    const auto elapsedNs = elapsed.count();
    if (elapsedNs <= 0) return 0;
    return static_cast<std::uint64_t>(
        (static_cast<long double>(packets) *
         static_cast<long double>(kPacketBits) *
         static_cast<long double>(kNanosecondsPerSecond)) /
        static_cast<long double>(elapsedNs));
}

std::uint64_t bytesForDuration(
    std::uint64_t bitrate,
    std::chrono::nanoseconds duration) noexcept {
    if (bitrate == 0 || duration.count() <= 0) return 0;
    return static_cast<std::uint64_t>(
        (static_cast<long double>(bitrate) *
         static_cast<long double>(duration.count())) /
        (8.0L * static_cast<long double>(kNanosecondsPerSecond)));
}

std::uint64_t packetOffsetNanoseconds(
    std::size_t packetIndex,
    std::uint64_t bitrate) noexcept {
    if (bitrate == 0 || packetIndex == 0) return 0;
    return static_cast<std::uint64_t>(
        (static_cast<long double>(packetIndex) *
         static_cast<long double>(kPacketBits) *
         static_cast<long double>(kNanosecondsPerSecond)) /
        static_cast<long double>(bitrate));
}

std::uint64_t nanosecondsToPcrTicks(std::uint64_t nanoseconds) noexcept {
#if defined(__SIZEOF_INT128__)
    const unsigned __int128 value =
        static_cast<unsigned __int128>(nanoseconds) * kPcrClockHz;
    return static_cast<std::uint64_t>(
        (value / kNanosecondsPerSecond) % kPcrTicksModulus);
#else
    const long double ticks =
        static_cast<long double>(nanoseconds) *
        static_cast<long double>(kPcrClockHz) /
        static_cast<long double>(kNanosecondsPerSecond);
    return static_cast<std::uint64_t>(ticks) % kPcrTicksModulus;
#endif
}

} // namespace

CbrTsPacer::CbrTsPacer(
    std::uint64_t targetBitrate,
    CbrPacingProfile profile)
    : profile_(profile),
      configuredTargetBitrate_(std::clamp(
          targetBitrate, kMinimumBitrate, kMaximumBitrate)),
      targetBitrate_(configuredTargetBitrate_) {
    if (targetBitrate == 0) {
        throw std::invalid_argument("CBR transport bitrate must be greater than zero");
    }
}

bool CbrTsPacer::enqueue(const Packet& packet) {
    if (packet[0] != kSyncByte) return false;

    // The shaper owns the NULL-packet budget. TVStreammerSAT5 uses the same
    // rule: preserve useful TS in the reservoir and regenerate PID 0x1fff in
    // exactly the slots left by the useful-data pace.
    if (isNullPacket(packet)) return true;

    // Capacity is checked before rate accounting so a producer retry after
    // backpressure cannot count the same TS packet twice.
    if ((queuedPackets_.size() + 1) * kPacketSize > maximumQueuedBytes()) {
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    if (tvStreammerSat5Profile()) {
        // SAT5 continuous-network profile owns its useful-packet clock via the
        // long-term arrival estimator + slow reservoir PLL. The configured
        // transport CBR must stay fixed; legacy 4-second auto-tune is disabled.
        observeTvStreammerSat5Arrival(packet, now);
    } else {
        observePayloadPacket(now);
    }
    queuedPackets_.push_back(packet);

    if (!started_) {
        if (tvStreammerSat5Profile()) {
            maybeStartTvStreammerSat5(now);
        } else {
            started_ = true;
            nextDeadline_ = now;
        }
    }
    return true;
}

bool CbrTsPacer::nextDatagram(
    std::chrono::steady_clock::time_point now,
    CbrDatagram& datagram) {
    if (!started_ || now < nextDeadline_) {
        return false;
    }

    const std::uint64_t periodNs =
        (kDatagramBits * kNanosecondsPerSecond) / targetBitrate_;
    const std::uint64_t latePeriods =
        tvStreammerSat5Profile() ? 4ULL : 1ULL;
    const auto maximumCatchup = std::chrono::nanoseconds(
        (std::max<std::uint64_t>)(periodNs * latePeriods, 1ULL));
    if (now - nextDeadline_ >= maximumCatchup) {
        // Match StableUdpOutput: scheduler stalls move only the physical sender
        // phase. Never dump a backlog of overdue UDP datagrams as a catch-up burst.
        nextDeadline_ = now;
        pacingRemainder_ = 0;
    }

    // SAT5 deliberately keeps media/PCR time independent from the physical
    // sender deadline. A late scheduler wake may rebase nextDeadline_ above, but
    // it must never jump PCR with it because upstream PTS/DTS did not jump.
    const auto datagramTime = tvStreammerSat5Profile()
        ? tvSatMediaTimeline_
        : nextDeadline_;
    if (tvStreammerSat5Profile()) {
        updateTvStreammerSat5Controller(now);
        fillTvStreammerSat5Datagram(datagramTime, datagram);
    } else {
        for (Packet& packet : datagram) {
            if (queuedPackets_.empty()) {
                makeNullPacket(packet);
            } else {
                packet = queuedPackets_.front();
                queuedPackets_.pop_front();
            }
        }
    }
    if (tvStreammerSat5Profile()) {
        advanceTvStreammerSat5MediaTimeline();
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

bool CbrTsPacer::canEnqueue(const Packet& packet) const noexcept {
    if (packet[0] != kSyncByte) return false;
    if (isNullPacket(packet)) return true;
    return (queuedPackets_.size() + 1) * kPacketSize <= maximumQueuedBytes();
}

void CbrTsPacer::pollStart(
    std::chrono::steady_clock::time_point now) noexcept {
    if (tvStreammerSat5Profile() && !started_) {
        maybeStartTvStreammerSat5(now);
    }
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
         static_cast<long double>(kPacketBits) *
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

    const std::uint64_t headroom = std::max<std::uint64_t>(
        recentPeak / 8U, 300000U);
    const std::uint64_t desired = std::clamp(
        roundUp100K(recentPeak + headroom),
        configuredTargetBitrate_, kMaximumBitrate);

    const bool cooldownFinished = !autoTuneApplied_ ||
        now - lastAutoTune_ >= kAutoTuneCooldown;
    if (cooldownFinished && recentPeak > 0) {
        // Preserve V10.8.148: adaptive CBR is raise-only. SAT5-compatible
        // reservoir control changes useful-packet spacing, never lowers the
        // configured transport bitrate.
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

void CbrTsPacer::advanceTvStreammerSat5MediaTimeline() noexcept {
    const std::uint64_t numerator = kDatagramBits * kNanosecondsPerSecond;
    std::uint64_t nanoseconds = numerator / targetBitrate_;
    tvSatMediaRemainder_ += numerator % targetBitrate_;
    if (tvSatMediaRemainder_ >= targetBitrate_) {
        nanoseconds += tvSatMediaRemainder_ / targetBitrate_;
        tvSatMediaRemainder_ %= targetBitrate_;
    }
    tvSatMediaTimeline_ += std::chrono::nanoseconds(nanoseconds);
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

bool CbrTsPacer::tvStreammerSat5Profile() const noexcept {
    return profile_ == CbrPacingProfile::TvStreammerSat5Network;
}

std::size_t CbrTsPacer::maximumQueuedBytes() const noexcept {
    return tvStreammerSat5Profile()
        ? kTvStreammerSat5MaximumQueuedBytes
        : kMaximumQueuedBytes;
}

void CbrTsPacer::observeTvStreammerSat5Arrival(
    const Packet& packet,
    std::chrono::steady_clock::time_point now) noexcept {
    if (tvSatFirstPacketTime_ == std::chrono::steady_clock::time_point{}) {
        tvSatFirstPacketTime_ = now;
    }

    PacketInfo info;
    if (inspectPacket(packet.data(), packet.size(), info) && info.hasPcr) {
        ++tvSatStartupPcrSamples_;
    }

    ++tvSatTotalArrivalPackets_;
    if (tvSatArrivalRateWindow_.empty() ||
        now - tvSatArrivalRateWindow_.back().first >= kTvSatArrivalSampleInterval) {
        tvSatArrivalRateWindow_.emplace_back(now, tvSatTotalArrivalPackets_);
    } else {
        // Keep the cumulative counter current without creating one node per TS packet.
        tvSatArrivalRateWindow_.back().second = tvSatTotalArrivalPackets_;
    }

    while (tvSatArrivalRateWindow_.size() > 2 &&
           now - tvSatArrivalRateWindow_.front().first > kTvSatArrivalRateWindow) {
        tvSatArrivalRateWindow_.pop_front();
    }
    if (tvSatArrivalRateWindow_.size() < 2) return;

    const auto& first = tvSatArrivalRateWindow_.front();
    const auto& last = tvSatArrivalRateWindow_.back();
    if (last.first <= first.first || last.second <= first.second) return;
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        last.first - first.first);
    if (elapsed < kTvSatArrivalRateMinimum) return;

    const std::uint64_t measured = bitrateForPackets(
        last.second - first.second, elapsed);
    if (measured == 0) return;

    const bool firstLock = tvSatEstimatedPayloadBitrate_ == 0;
    // Match StableUdpOutput networkLongTermArrivalBitrate: the 15-second window
    // already removes TCP/SRT delivery bursts, then a 3/4 EWMA follows only
    // genuine long-term service-rate changes.
    tvSatEstimatedPayloadBitrate_ = firstLock
        ? measured
        : (tvSatEstimatedPayloadBitrate_ * 3ULL + measured) / 4ULL;
    if (firstLock) {
        std::cerr << "CBR TVSTREAMMERSAT5 network rate lock"
                  << " source_rate=arrival_15s"
                  << " useful_kbps=" << (tvSatEstimatedPayloadBitrate_ / 1000ULL)
                  << " minimum_window_ms=5000"
                  << " PCR_density=diagnostic_only"
                  << std::endl;
    }
}

void CbrTsPacer::maybeStartTvStreammerSat5(
    std::chrono::steady_clock::time_point now) noexcept {
    if (started_ ||
        tvSatFirstPacketTime_ == std::chrono::steady_clock::time_point{} ||
        queuedPackets_.empty()) {
        return;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now - tvSatFirstPacketTime_);
    if (elapsed < kTvSatStartupReservoir) return;

    // Match StableUdpOutput waitForInitialPackets(): five seconds of wall-clock
    // reservoir plus five PCR samples. Do not require a second derived byte
    // threshold -- the original SAT5 sender starts from the bytes actually
    // accumulated during that five-second interval.
    if (tvSatStartupPcrSamples_ < kTvSatStartupMinimumPcrSamples) return;

    const std::uint64_t queuedBytes =
        static_cast<std::uint64_t>(queuedPackets_.size()) * kPacketSize;
    const std::uint64_t startupRate = bitrateForPackets(
        static_cast<std::uint64_t>(queuedPackets_.size()), elapsed);
    if (startupRate > 0) {
        tvSatEstimatedPayloadBitrate_ = startupRate;
    }
    if (tvSatEstimatedPayloadBitrate_ == 0) return;

    started_ = true;
    nextDeadline_ = now;
    tvSatMediaTimeline_ = now;
    tvSatMediaRemainder_ = 0;
    tvSatLastControllerUpdate_ = now;
    const std::uint64_t maximumUsefulBitrate = targetBitrate_ > 100000ULL
        ? targetBitrate_ - 100000ULL
        : targetBitrate_;
    tvSatPllBaseBitrate_ = (std::min)(
        tvSatEstimatedPayloadBitrate_, maximumUsefulBitrate);
    tvSatRealPaceBitrate_ = tvSatPllBaseBitrate_;
    tvSatRealTokenAccumulator_ = 0;

    if (!tvSatStartLogged_) {
        tvSatStartLogged_ = true;
        std::cerr << "CBR TVSTREAMMERSAT5 profile start"
                  << " target_kbps=" << (targetBitrate_ / 1000ULL)
                  << " estimated_payload_kbps="
                  << (tvSatEstimatedPayloadBitrate_ / 1000ULL)
                  << " real_pace_kbps=" << (tvSatRealPaceBitrate_ / 1000ULL)
                  << " startup_reservoir_ms=5000"
                  << " startup_kb=" << (queuedBytes / 1024ULL)
                  << " startup_pcr_samples=" << tvSatStartupPcrSamples_
                  << " buffer_limit_mb=32"
                  << " datagram_bytes="
                  << (kPacketsPerCbrDatagram * kPacketSize)
                  << " transport_auto_tune=off"
                  << " source_pcr=stripped-after-lock"
                  << " synthetic_pcr=20ms"
                  << " media_clock=independent"
                  << std::endl;
    }
}

void CbrTsPacer::updateTvStreammerSat5Controller(
    std::chrono::steady_clock::time_point now) noexcept {
    if (tvSatEstimatedPayloadBitrate_ == 0) return;
    if (tvSatLastControllerUpdate_ != std::chrono::steady_clock::time_point{} &&
        now - tvSatLastControllerUpdate_ < kTvSatControllerUpdate) {
        return;
    }
    tvSatLastControllerUpdate_ = now;

    const std::uint64_t maximumUsefulBitrate = targetBitrate_ > 100000ULL
        ? targetBitrate_ - 100000ULL
        : targetBitrate_;
    const std::uint64_t sourceLimited = (std::min)(
        tvSatEstimatedPayloadBitrate_, maximumUsefulBitrate);

    if (tvSatPllBaseBitrate_ == 0) {
        tvSatPllBaseBitrate_ = sourceLimited;
    } else {
        // SAT5 continuous-network PLL: follow a noisy long-term source estimate
        // at only 1/64 per controller update.
        tvSatPllBaseBitrate_ =
            (tvSatPllBaseBitrate_ * (kTvSatPllFollowDivisor - 1ULL) + sourceLimited) /
            kTvSatPllFollowDivisor;
    }
    tvSatPllBaseBitrate_ = (std::min)(tvSatPllBaseBitrate_, maximumUsefulBitrate);

    const std::uint64_t bufferBytes =
        static_cast<std::uint64_t>(queuedPackets_.size()) * kPacketSize;
    const std::uint64_t targetBufferBytes = (std::max<std::uint64_t>)(
        kPacketsPerCbrDatagram * kPacketSize * 32ULL,
        bytesForDuration(
            tvSatPllBaseBitrate_,
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                kTvSatTargetReservoir)));

    std::int64_t errorBytes = 0;
    if (bufferBytes >= targetBufferBytes) {
        const std::uint64_t diff = bufferBytes - targetBufferBytes;
        errorBytes = static_cast<std::int64_t>((std::min<std::uint64_t>)(
            diff, static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())));
    } else {
        const std::uint64_t diff = targetBufferBytes - bufferBytes;
        errorBytes = -static_cast<std::int64_t>((std::min<std::uint64_t>)(
            diff, static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())));
    }

    // 1% pace correction for a 100% reservoir occupancy error, hard-limited
    // to +/-1%, exactly the SAT5 continuous-network controller policy.
    std::int64_t correction = 0;
    if (targetBufferBytes > 0) {
#if defined(__SIZEOF_INT128__)
        correction = static_cast<std::int64_t>(
            (static_cast<__int128>(tvSatPllBaseBitrate_) * errorBytes) /
            (static_cast<__int128>(targetBufferBytes) * 100));
#else
        correction = static_cast<std::int64_t>(
            (static_cast<long double>(tvSatPllBaseBitrate_) *
             static_cast<long double>(errorBytes)) /
            (static_cast<long double>(targetBufferBytes) * 100.0L));
#endif
    }
    const std::int64_t maximumCorrection = static_cast<std::int64_t>(
        tvSatPllBaseBitrate_ * kTvSatPllCorrectionPermille / 1000ULL);
    correction = std::clamp<std::int64_t>(
        correction, -maximumCorrection, maximumCorrection);

    std::int64_t desired = static_cast<std::int64_t>(tvSatPllBaseBitrate_) + correction;
    desired = std::clamp<std::int64_t>(
        desired, 0, static_cast<std::int64_t>(maximumUsefulBitrate));

    const std::uint64_t maximumStep = (std::max<std::uint64_t>)(
        1000ULL, tvSatPllBaseBitrate_ * kTvSatPllStepPermille / 1000ULL);
    const std::int64_t current = static_cast<std::int64_t>(tvSatRealPaceBitrate_);
    const std::int64_t lower = current > static_cast<std::int64_t>(maximumStep)
        ? current - static_cast<std::int64_t>(maximumStep)
        : 0;
    const std::int64_t upper = (std::min)(
        static_cast<std::int64_t>(maximumUsefulBitrate),
        current + static_cast<std::int64_t>(maximumStep));
    tvSatRealPaceBitrate_ = static_cast<std::uint64_t>(
        std::clamp<std::int64_t>(desired, lower, upper));
}

void CbrTsPacer::fillTvStreammerSat5Datagram(
    std::chrono::steady_clock::time_point datagramTime,
    CbrDatagram& datagram) noexcept {
    for (std::size_t index = 0; index < datagram.size(); ++index) {
        const auto slotTime = datagramTime + std::chrono::nanoseconds(
            packetOffsetNanoseconds(index, targetBitrate_));

        // StableUdpOutput accrues real-packet entitlement on every transport
        // slot, including slots occupied by its periodic PCR-only packet.
        tvSatRealTokenAccumulator_ += tvSatRealPaceBitrate_;

        // TVStreamer5 continuous SRT/HTTP owns a synthetic 20 ms PCR domain.
        // Periodic PCR consumes an otherwise NULL CBR slot; the real-packet
        // token remains accrued so useful media is not throttled by PCR inserts.
        if (tvSatPcrInitialized_ && slotTime >= tvSatNextPeriodicPcrTime_) {
            makePeriodicPcrPacket(datagram[index], slotTime);
            do {
                tvSatNextPeriodicPcrTime_ += kTvSatPeriodicPcrInterval;
            } while (tvSatNextPeriodicPcrTime_ <= slotTime);
            continue;
        }

        bool sendReal = false;
        if (targetBitrate_ > 0 && tvSatRealTokenAccumulator_ >= targetBitrate_) {
            tvSatRealTokenAccumulator_ -= targetBitrate_;
            sendReal = !queuedPackets_.empty();
            if (!sendReal) {
                // Never accumulate a catch-up burst during an upstream gap.
                tvSatRealTokenAccumulator_ = (std::min)(
                    tvSatRealTokenAccumulator_, targetBitrate_ - std::uint64_t{1});
            }
        }

        if (sendReal) {
            datagram[index] = queuedPackets_.front();
            queuedPackets_.pop_front();
            processTvStreammerSat5RealPacket(datagram[index], slotTime);
        } else {
            makeNullPacket(datagram[index]);
        }
    }
}

void CbrTsPacer::processTvStreammerSat5RealPacket(
    Packet& packet,
    std::chrono::steady_clock::time_point slotTime) noexcept {
    PacketInfo info;
    if (!inspectPacket(packet.data(), packet.size(), info)) return;

    if (info.hasPcr) {
        if (!tvSatPcrInitialized_) {
            // Match StableUdpOutput 203.07/202.74: the first provider PCR is
            // the phase anchor. Do not consume startup media or advance phase.
            tvSatPcrInitialized_ = true;
            tvSatPcrPid_ = info.pid;
            tvSatPcrOriginTicks_ =
                (info.pcrBase90k * 300ULL) % kPcrTicksModulus;
            tvSatPcrOriginTime_ = slotTime;
            tvSatNextPeriodicPcrTime_ = slotTime + kTvSatPeriodicPcrInterval;
            writePcr(packet, tvSatPcrOriginTicks_);
            std::cerr << "CBR TVSTREAMMERSAT5 PCR lock"
                      << " pid=" << tvSatPcrPid_
                      << " mode=synthetic-tvstreamer5-20ms"
                      << " source_pcr=stripped-after-lock"
                      << " media_clock=independent"
                      << std::endl;
        } else if (info.pid == tvSatPcrPid_) {
            clearPcrFlag(packet);
        }
    }

    // Adaptation-only PCR packets do not advance payload continuity. Periodic
    // PCR packets reuse the most recently observed payload CC on the PCR PID.
    if (tvSatPcrInitialized_ && info.pid == tvSatPcrPid_ && info.hasPayload) {
        tvSatPcrPidContinuityCounter_ = info.continuityCounter;
        tvSatPcrPidContinuityValid_ = true;
    }
}

void CbrTsPacer::makePeriodicPcrPacket(
    Packet& packet,
    std::chrono::steady_clock::time_point slotTime) noexcept {
    packet.fill(0xff);
    packet[0] = kSyncByte;
    packet[1] = static_cast<std::uint8_t>((tvSatPcrPid_ >> 8) & 0x1fU);
    packet[2] = static_cast<std::uint8_t>(tvSatPcrPid_ & 0xffU);
    // Adaptation-only packets do not advance payload continuity.
    packet[3] = static_cast<std::uint8_t>(
        0x20U | (tvSatPcrPidContinuityValid_
            ? (tvSatPcrPidContinuityCounter_ & 0x0fU)
            : 0U));
    packet[4] = 183;
    packet[5] = 0x10; // PCR flag
    writePcr(packet, pcrTicksAt(slotTime));
}

std::uint64_t CbrTsPacer::pcrTicksAt(
    std::chrono::steady_clock::time_point slotTime) const noexcept {
    if (!tvSatPcrInitialized_) return 0;
    const auto elapsed = slotTime >= tvSatPcrOriginTime_
        ? std::chrono::duration_cast<std::chrono::nanoseconds>(
            slotTime - tvSatPcrOriginTime_).count()
        : 0;
    return (tvSatPcrOriginTicks_ +
            nanosecondsToPcrTicks(static_cast<std::uint64_t>(elapsed))) %
        kPcrTicksModulus;
}

void CbrTsPacer::clearPcrFlag(Packet& packet) noexcept {
    if (packet[0] != kSyncByte) return;
    const std::uint8_t adaptationFieldControl =
        static_cast<std::uint8_t>((packet[3] >> 4) & 0x03U);
    if (adaptationFieldControl != 2U && adaptationFieldControl != 3U) return;
    if (packet[4] == 0) return;
    packet[5] = static_cast<std::uint8_t>(packet[5] & ~0x10U);
}

void CbrTsPacer::writePcr(Packet& packet, std::uint64_t pcrTicks) noexcept {
    if (packet.size() < 12 || packet[0] != kSyncByte ||
        (packet[3] & 0x20U) == 0 || packet[4] < 7 ||
        (packet[5] & 0x10U) == 0) {
        return;
    }
    pcrTicks %= kPcrTicksModulus;
    const std::uint64_t base = pcrTicks / 300ULL;
    const std::uint16_t extension = static_cast<std::uint16_t>(pcrTicks % 300ULL);
    packet[6] = static_cast<std::uint8_t>((base >> 25) & 0xffU);
    packet[7] = static_cast<std::uint8_t>((base >> 17) & 0xffU);
    packet[8] = static_cast<std::uint8_t>((base >> 9) & 0xffU);
    packet[9] = static_cast<std::uint8_t>((base >> 1) & 0xffU);
    packet[10] = static_cast<std::uint8_t>(
        ((base & 0x1ULL) << 7) | 0x7eU | ((extension >> 8) & 0x1U));
    packet[11] = static_cast<std::uint8_t>(extension & 0xffU);
}

} // namespace dvbstreamer5::media::mpegts
