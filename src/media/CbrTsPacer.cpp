#include "media/CbrTsPacer.h"

#include <algorithm>
#include <stdexcept>

namespace tvs::media::mpegts {
namespace {

constexpr std::uint64_t kNanosecondsPerSecond = 1000000000ULL;
constexpr std::uint64_t kDatagramBits =
    kPacketsPerCbrDatagram * kPacketSize * 8ULL;

} // namespace

CbrTsPacer::CbrTsPacer(std::uint64_t targetBitrate)
    : targetBitrate_(std::clamp(
          targetBitrate, kMinimumBitrate, kMaximumBitrate)) {
    if (targetBitrate == 0) {
        throw std::invalid_argument("CBR transport bitrate must be greater than zero");
    }
}

bool CbrTsPacer::enqueue(const Packet& packet) {
    if (packet[0] != kSyncByte ||
        (queuedPackets_.size() + 1) * kPacketSize > kMaximumQueuedBytes) {
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

    const auto interval = std::chrono::nanoseconds(
        kDatagramBits * kNanosecondsPerSecond / targetBitrate_);
    if (now - nextDeadline_ > interval * 2) {
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

} // namespace tvs::media::mpegts
