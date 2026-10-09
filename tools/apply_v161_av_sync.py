from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected 1 match, got {count}")
    return text.replace(old, new, 1)

# CbrTsPacer.h
path = Path("src/media/CbrTsPacer.h")
text = path.read_text()
text = replace_once(
    text,
    "#include <deque>\n",
    "#include <deque>\n#include <utility>\n",
    "include utility")
text = replace_once(
    text,
    "    std::chrono::steady_clock::time_point tvSatArrivalWindowStart_ {};\n"
    "    std::chrono::steady_clock::time_point tvSatLastControllerUpdate_ {};\n"
    "    std::uint64_t tvSatArrivalPacketsInWindow_ = 0;\n"
    "    std::uint64_t tvSatEstimatedPayloadBitrate_ = 0;\n"
    "    std::uint64_t tvSatRealPaceBitrate_ = 0;\n",
    "    std::chrono::steady_clock::time_point tvSatArrivalWindowStart_ {};\n"
    "    std::chrono::steady_clock::time_point tvSatLastControllerUpdate_ {};\n"
    "    std::uint64_t tvSatArrivalPacketsInWindow_ = 0;\n"
    "    std::uint64_t tvSatTotalArrivalPackets_ = 0;\n"
    "    std::deque<std::pair<std::chrono::steady_clock::time_point, std::uint64_t>>\n"
    "        tvSatArrivalRateWindow_;\n"
    "    std::uint64_t tvSatEstimatedPayloadBitrate_ = 0;\n"
    "    std::uint64_t tvSatPllBaseBitrate_ = 0;\n"
    "    std::uint64_t tvSatRealPaceBitrate_ = 0;\n",
    "SAT5 PLL state")
path.write_text(text)

# CbrTsPacer.cpp
path = Path("src/media/CbrTsPacer.cpp")
text = path.read_text()
text = replace_once(
    text,
    "constexpr auto kTvSatStartupReservoir = std::chrono::seconds(5);\n"
    "constexpr auto kTvSatRateSample = std::chrono::milliseconds(500);\n"
    "constexpr auto kTvSatControllerUpdate = std::chrono::milliseconds(100);\n"
    "constexpr auto kTvSatTargetReservoir = std::chrono::milliseconds(2500);\n"
    "constexpr auto kTvSatLowReservoir = std::chrono::milliseconds(800);\n"
    "constexpr auto kTvSatCorrectionHorizon = std::chrono::seconds(6);\n",
    "constexpr auto kTvSatStartupReservoir = std::chrono::seconds(5);\n"
    "// SAT5 continuous-network profile (202.22): rate is measured over a\n"
    "// 15-second arrival window after at least five seconds of evidence.\n"
    "constexpr auto kTvSatArrivalSampleInterval = std::chrono::milliseconds(500);\n"
    "constexpr auto kTvSatArrivalRateWindow = std::chrono::seconds(15);\n"
    "constexpr auto kTvSatArrivalRateMinimum = std::chrono::seconds(5);\n"
    "// The reservoir PLL intentionally reacts much more slowly than the input\n"
    "// reader: one update every two seconds, max +/-1% correction and 0.1% step.\n"
    "constexpr auto kTvSatControllerUpdate = std::chrono::seconds(2);\n"
    "constexpr auto kTvSatTargetReservoir = std::chrono::milliseconds(2500);\n"
    "constexpr std::uint64_t kTvSatPllCorrectionPermille = 10ULL;\n"
    "constexpr std::uint64_t kTvSatPllStepPermille = 1ULL;\n"
    "constexpr std::uint64_t kTvSatPllFollowDivisor = 64ULL;\n",
    "SAT5 network PLL constants")

start = text.index("void CbrTsPacer::observeTvStreammerSat5Arrival(")
end = text.index("void CbrTsPacer::maybeStartTvStreammerSat5(", start)
new_observe = '''void CbrTsPacer::observeTvStreammerSat5Arrival(
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

'''
text = text[:start] + new_observe + text[end:]

text = replace_once(
    text,
    "    tvSatRealPaceBitrate_ = (std::min)(\n"
    "        tvSatEstimatedPayloadBitrate_, maximumUsefulBitrate);\n"
    "    tvSatRealTokenAccumulator_ = 0;\n",
    "    tvSatPllBaseBitrate_ = (std::min)(\n"
    "        tvSatEstimatedPayloadBitrate_, maximumUsefulBitrate);\n"
    "    tvSatRealPaceBitrate_ = tvSatPllBaseBitrate_;\n"
    "    tvSatRealTokenAccumulator_ = 0;\n",
    "seed SAT5 PLL base")

start = text.index("void CbrTsPacer::updateTvStreammerSat5Controller(")
end = text.index("void CbrTsPacer::fillTvStreammerSat5Datagram(", start)
new_controller = '''void CbrTsPacer::updateTvStreammerSat5Controller(
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

'''
text = text[:start] + new_controller + text[end:]
path.write_text(text)

# Version
path = Path("src/AppVersion.h")
text = path.read_text()
text = replace_once(text, 'kProgramVersion = "10.8.160"', 'kProgramVersion = "10.8.161"', "version")
path.write_text(text)
