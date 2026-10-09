from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected 1 match, got {count}")
    return text.replace(old, new, 1)

# --- CbrTsPacer.h ---
path = Path("src/media/CbrTsPacer.h")
text = path.read_text()
text = replace_once(
    text,
    "    std::uint64_t targetBitrate() const noexcept;\n"
    "    std::size_t queuedPackets() const noexcept;\n"
    "    CbrPacingProfile profile() const noexcept { return profile_; }\n",
    "    std::uint64_t targetBitrate() const noexcept;\n"
    "    std::size_t queuedPackets() const noexcept;\n"
    "    bool canEnqueue(const Packet& packet) const noexcept;\n"
    "    // Dedicated SAT5 sender threads poll this during the five-second cold\n"
    "    // reservoir so startup is not dependent on another producer enqueue.\n"
    "    void pollStart(std::chrono::steady_clock::time_point now) noexcept;\n"
    "    CbrPacingProfile profile() const noexcept { return profile_; }\n",
    "CbrTsPacer public startup API")
path.write_text(text)

# --- CbrTsPacer.cpp ---
path = Path("src/media/CbrTsPacer.cpp")
text = path.read_text()

text = replace_once(
    text,
    "    const auto now = std::chrono::steady_clock::now();\n"
    "    observePayloadPacket(now);\n"
    "    if (tvStreammerSat5Profile()) {\n"
    "        observeTvStreammerSat5Arrival(packet, now);\n"
    "    }\n\n"
    "    if ((queuedPackets_.size() + 1) * kPacketSize > maximumQueuedBytes()) {\n"
    "        return false;\n"
    "    }\n"
    "    queuedPackets_.push_back(packet);\n",
    "    // Capacity is checked before rate accounting so a producer retry after\n"
    "    // backpressure cannot count the same TS packet twice.\n"
    "    if ((queuedPackets_.size() + 1) * kPacketSize > maximumQueuedBytes()) {\n"
    "        return false;\n"
    "    }\n\n"
    "    const auto now = std::chrono::steady_clock::now();\n"
    "    observePayloadPacket(now);\n"
    "    if (tvStreammerSat5Profile()) {\n"
    "        observeTvStreammerSat5Arrival(packet, now);\n"
    "    }\n"
    "    queuedPackets_.push_back(packet);\n",
    "capacity-before-accounting")

text = replace_once(
    text,
    "std::size_t CbrTsPacer::queuedPackets() const noexcept {\n"
    "    return queuedPackets_.size();\n"
    "}\n",
    "std::size_t CbrTsPacer::queuedPackets() const noexcept {\n"
    "    return queuedPackets_.size();\n"
    "}\n\n"
    "bool CbrTsPacer::canEnqueue(const Packet& packet) const noexcept {\n"
    "    if (packet[0] != kSyncByte) return false;\n"
    "    if (isNullPacket(packet)) return true;\n"
    "    return (queuedPackets_.size() + 1) * kPacketSize <= maximumQueuedBytes();\n"
    "}\n\n"
    "void CbrTsPacer::pollStart(\n"
    "    std::chrono::steady_clock::time_point now) noexcept {\n"
    "    if (tvStreammerSat5Profile() && !started_) {\n"
    "        maybeStartTvStreammerSat5(now);\n"
    "    }\n"
    "}\n",
    "queue/start public helpers")

text = replace_once(
    text,
    "        tvSatEstimatedPayloadBitrate_ = tvSatEstimatedPayloadBitrate_ == 0\n"
    "            ? measured\n"
    "            : (tvSatEstimatedPayloadBitrate_ * 3ULL + measured) / 4ULL;\n",
    "        // StableUdpOutput follows network-rate changes slowly. Keep the\n"
    "        // same 8-sample EWMA so read chunking/GOP bursts do not move the\n"
    "        // useful-packet clock abruptly.\n"
    "        tvSatEstimatedPayloadBitrate_ = tvSatEstimatedPayloadBitrate_ == 0\n"
    "            ? measured\n"
    "            : (tvSatEstimatedPayloadBitrate_ * 7ULL + measured) / 8ULL;\n",
    "SAT5 rate EWMA")

start = text.index("void CbrTsPacer::maybeStartTvStreammerSat5(")
end = text.index("void CbrTsPacer::updateTvStreammerSat5Controller(", start)
new_start = '''void CbrTsPacer::maybeStartTvStreammerSat5(
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
    tvSatLastControllerUpdate_ = now;
    const std::uint64_t maximumUsefulBitrate = targetBitrate_ > 100000ULL
        ? targetBitrate_ - 100000ULL
        : targetBitrate_;
    tvSatRealPaceBitrate_ = (std::min)(
        tvSatEstimatedPayloadBitrate_, maximumUsefulBitrate);
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
                  << std::endl;
    }
}

'''
text = text[:start] + new_start + text[end:]

text = replace_once(
    text,
    "    desired = std::clamp(\n"
    "        desired,\n"
    "        0.0L,\n"
    "        static_cast<long double>(targetBitrate_));\n"
    "    tvSatRealPaceBitrate_ = static_cast<std::uint64_t>(desired);\n",
    "    const std::uint64_t maximumUsefulBitrate = targetBitrate_ > 100000ULL\n"
    "        ? targetBitrate_ - 100000ULL\n"
    "        : targetBitrate_;\n"
    "    desired = std::clamp(\n"
    "        desired,\n"
    "        0.0L,\n"
    "        static_cast<long double>(maximumUsefulBitrate));\n"
    "    tvSatRealPaceBitrate_ = static_cast<std::uint64_t>(desired);\n",
    "SAT5 useful-rate ceiling")

start = text.index("void CbrTsPacer::processTvStreammerSat5RealPacket(")
end = text.index("void CbrTsPacer::makePeriodicPcrPacket(", start)
new_process = '''void CbrTsPacer::processTvStreammerSat5RealPacket(
    Packet& packet,
    std::chrono::steady_clock::time_point slotTime) noexcept {
    PacketInfo info;
    if (!inspectPacket(packet.data(), packet.size(), info)) return;

    bool firstPcrLock = false;
    if (!tvSatPcrInitialized_ && info.hasPcr) {
        tvSatPcrInitialized_ = true;
        tvSatPcrPid_ = info.pid;
        // TransportStream::PacketInfo exposes PCR base at 90 kHz. Preserve the
        // 9-bit PCR extension as SAT5 does so the initial 27 MHz phase is exact.
        const std::uint64_t extension =
            ((static_cast<std::uint64_t>(packet[10] & 0x01U) << 8) |
             static_cast<std::uint64_t>(packet[11]));
        tvSatPcrOriginTicks_ =
            (info.pcrBase90k * 300ULL + extension) % kPcrTicksModulus;
        tvSatPcrOriginTime_ = slotTime;
        tvSatNextPeriodicPcrTime_ = slotTime + kTvSatPeriodicPcrInterval;
        firstPcrLock = true;
        std::cerr << "CBR TVSTREAMMERSAT5 PCR lock"
                  << " pid=" << tvSatPcrPid_
                  << " cadence_ms=20"
                  << " mode=synthetic-periodic"
                  << std::endl;
    }

    if (tvSatPcrInitialized_ && info.pid == tvSatPcrPid_) {
        if (info.hasPayload) {
            tvSatPcrPidContinuityCounter_ = info.continuityCounter;
            tvSatPcrPidContinuityValid_ = true;
        }
        if (info.hasPcr) {
            if (firstPcrLock) {
                // The first provider PCR anchors the new continuous domain.
                writePcr(packet, tvSatPcrOriginTicks_);
            } else if ((packet[3] & 0x20U) != 0 && packet[4] >= 1) {
                // SAT5 removes later provider PCRs; their bytes remain legal
                // adaptation stuffing. A single periodic adaptation-only PCR
                // packet every 20 ms is then the authoritative output clock.
                packet[5] = static_cast<std::uint8_t>(packet[5] & ~0x10U);
            }
        }
    }
}

'''
text = text[:start] + new_process + text[end:]
path.write_text(text)

# --- dedicated SAT5 CBR sender (header-only, no CMake target change) ---
Path("src/media/TvSatCbrOutputWorker.h").write_text(r'''#pragma once

#include "media/CbrTsPacer.h"
#include "media/UdpSocket.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace dvbstreamer5::media::network {

// Dedicated UDP-CBR sender for the TVStreammerSAT5 continuous SRT/HTTP profile.
// Input/remap/CA may stall or arrive in bursts without moving the output clock:
// this thread owns the absolute steady-clock deadlines and sends one 7x188
// datagram per transport interval. Queue backpressure matches SAT5's bounded
// reservoir instead of dropping packets or dumping overdue catch-up bursts.
class TvSatCbrOutputWorker {
public:
    using Observer = std::function<void(const std::uint8_t*, std::size_t)>;
    using ErrorCallback = std::function<void(const std::string&)>;

    TvSatCbrOutputWorker(
        UdpSocket* socket,
        std::size_t outputIndex,
        std::uint64_t targetBitrate,
        std::atomic<std::uint64_t>* outputBytes,
        std::atomic<bool>* relayRunning,
        Observer observer,
        ErrorCallback errorCallback)
        : socket_(socket),
          outputIndex_(outputIndex),
          pacer_(targetBitrate, mpegts::CbrPacingProfile::TvStreammerSat5Network),
          outputBytes_(outputBytes),
          relayRunning_(relayRunning),
          observer_(std::move(observer)),
          errorCallback_(std::move(errorCallback)) {
        if (!socket_ || !outputBytes_ || !relayRunning_) {
            throw std::invalid_argument("TVStreammerSAT5 CBR worker requires valid relay objects");
        }
        senderThread_ = std::thread(&TvSatCbrOutputWorker::sendLoop, this);
    }

    ~TvSatCbrOutputWorker() { stop(); }

    TvSatCbrOutputWorker(const TvSatCbrOutputWorker&) = delete;
    TvSatCbrOutputWorker& operator=(const TvSatCbrOutputWorker&) = delete;

    bool enqueue(const std::vector<mpegts::Packet>& packets) {
        bool notifyStartup = false;
        for (const auto& packet : packets) {
            if (packet[0] != mpegts::kSyncByte) return false;

            std::unique_lock<std::mutex> lock(mutex_);
            while (!stopping_ && relayRunning_->load(std::memory_order_acquire) &&
                   !pacer_.canEnqueue(packet)) {
                // Timed wait is deliberate: NativeUdpRelay::stop() owns the
                // outer relay flag and cannot directly signal this local worker.
                queueSpace_.wait_for(lock, std::chrono::milliseconds(20));
            }
            if (stopping_ || !relayRunning_->load(std::memory_order_acquire)) {
                return false;
            }

            const bool wasStarted = pacer_.started();
            if (!pacer_.enqueue(packet)) return false;
            notifyStartup = notifyStartup || (!wasStarted && pacer_.started());
        }
        // During the cold five-second reservoir the sender polls startup every
        // 20 ms. This notification is only needed to reduce first-start latency.
        if (notifyStartup) senderWake_.notify_one();
        return true;
    }

    void stop() noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) {
                // Still join below if another caller set the flag first.
            } else {
                stopping_ = true;
            }
        }
        senderWake_.notify_all();
        queueSpace_.notify_all();
        if (senderThread_.joinable() &&
            senderThread_.get_id() != std::this_thread::get_id()) {
            senderThread_.join();
        }
    }

private:
    void fail(const std::string& message) noexcept {
        relayRunning_->store(false, std::memory_order_release);
        if (errorCallback_) {
            try { errorCallback_(message); } catch (...) {}
        }
        queueSpace_.notify_all();
        senderWake_.notify_all();
    }

    void sendLoop() noexcept {
        try {
            std::unique_lock<std::mutex> lock(mutex_);
            while (!stopping_ && relayRunning_->load(std::memory_order_acquire)) {
                auto now = std::chrono::steady_clock::now();
                pacer_.pollStart(now);
                if (!pacer_.started()) {
                    senderWake_.wait_for(
                        lock, std::chrono::milliseconds(20), [this] {
                            return stopping_ ||
                                !relayRunning_->load(std::memory_order_acquire);
                        });
                    continue;
                }

                const auto deadline = pacer_.nextDeadline();
                if (now < deadline) {
                    senderWake_.wait_until(lock, deadline, [this] {
                        return stopping_ ||
                            !relayRunning_->load(std::memory_order_acquire);
                    });
                    if (stopping_ ||
                        !relayRunning_->load(std::memory_order_acquire)) {
                        break;
                    }
                }

                mpegts::CbrDatagram datagram {};
                const std::size_t queuedBefore = pacer_.queuedPackets();
                if (!pacer_.nextDatagram(
                        std::chrono::steady_clock::now(), datagram)) {
                    continue;
                }
                const std::size_t queuedAfter = pacer_.queuedPackets();
                const std::uint64_t targetBitrate = pacer_.targetBitrate();
                if (queuedAfter < queuedBefore) queueSpace_.notify_all();

                constexpr std::size_t kDatagramBytes =
                    mpegts::kPacketsPerCbrDatagram * mpegts::kPacketSize;
                static_assert(sizeof(mpegts::CbrDatagram) == kDatagramBytes,
                    "SAT5 CBR datagram storage must be contiguous");
                const auto* bytes = reinterpret_cast<const std::uint8_t*>(
                    datagram.data());

                lock.unlock();
                std::string sendError;
                if (!socket_->send(bytes, kDatagramBytes, sendError)) {
                    fail(sendError.empty()
                        ? "TVStreammerSAT5 UDP CBR output send failed"
                        : "TVStreammerSAT5 UDP CBR output send failed: " + sendError);
                    lock.lock();
                    stopping_ = true;
                    break;
                }
                outputBytes_->fetch_add(kDatagramBytes, std::memory_order_relaxed);

                if (observer_) {
                    try {
                        observer_(bytes, kDatagramBytes);
                    } catch (const std::exception& exception) {
                        fail(std::string("TVStreammerSAT5 CBR observer failed: ") +
                             exception.what());
                        lock.lock();
                        stopping_ = true;
                        break;
                    } catch (...) {
                        fail("TVStreammerSAT5 CBR observer failed");
                        lock.lock();
                        stopping_ = true;
                        break;
                    }
                }

                if (!firstSendLogged_) {
                    firstSendLogged_ = true;
                    std::cerr << "NATIVE UDP OUTPUT first_send index="
                              << outputIndex_
                              << " type=udp-cbr"
                              << " profile=tvstreammersat5-network"
                              << " queued_packets=" << queuedAfter
                              << " target_kbps=" << (targetBitrate / 1000ULL)
                              << " sender_thread=dedicated"
                              << std::endl;
                }
                lock.lock();
            }
        } catch (const std::exception& exception) {
            fail(std::string("TVStreammerSAT5 CBR sender exception: ") +
                 exception.what());
        } catch (...) {
            fail("TVStreammerSAT5 CBR sender exception");
        }
    }

    UdpSocket* socket_ = nullptr;
    std::size_t outputIndex_ = 0;
    mpegts::CbrTsPacer pacer_;
    std::atomic<std::uint64_t>* outputBytes_ = nullptr;
    std::atomic<bool>* relayRunning_ = nullptr;
    Observer observer_;
    ErrorCallback errorCallback_;

    std::mutex mutex_;
    std::condition_variable senderWake_;
    std::condition_variable queueSpace_;
    bool stopping_ = false;
    bool firstSendLogged_ = false;
    std::thread senderThread_;
};

} // namespace dvbstreamer5::media::network
''')

# --- NativeUdpRelay.cpp ---
path = Path("src/media/NativeUdpRelay.cpp")
text = path.read_text()
text = replace_once(
    text,
    '#include "media/CbrTsPacer.h"\n#include "media/RtpMpegTs.h"\n',
    '#include "media/CbrTsPacer.h"\n#include "media/TvSatCbrOutputWorker.h"\n#include "media/RtpMpegTs.h"\n',
    "NativeUdpRelay worker include")

text = replace_once(
    text,
    "        std::unique_ptr<dvbstreamer5::media::rtp::MpegTsPacketizer> packetizer;\n"
    "        std::unique_ptr<dvbstreamer5::media::mpegts::CbrTsPacer> cbrPacer;\n",
    "        std::unique_ptr<dvbstreamer5::media::rtp::MpegTsPacketizer> packetizer;\n"
    "        std::unique_ptr<dvbstreamer5::media::mpegts::CbrTsPacer> cbrPacer;\n"
    "        std::unique_ptr<TvSatCbrOutputWorker> tvSatCbrWorker;\n",
    "OutputWorker SAT5 member")

text = replace_once(
    text,
    "    std::vector<OutputWorker> outputs;\n"
    "    outputs.reserve(config_.outputs.size());\n"
    "    auto seed = static_cast<std::uint64_t>(\n"
    "        std::chrono::steady_clock::now().time_since_epoch().count());\n",
    "    std::vector<OutputWorker> outputs;\n"
    "    outputs.reserve(config_.outputs.size());\n\n"
    "    // V10.8.159: decide observer sharing before constructing the dedicated\n"
    "    // SAT5 sender so that exactly one already-shaped CBR datagram stream is\n"
    "    // published to HTTP/SRT/HLS/RTSP/RTMP consumers.\n"
    "    std::size_t observedCbrOutputIndex = config_.outputs.size();\n"
    "    if (config_.paceObservedTransport && config_.observeTransport) {\n"
    "        for (std::size_t index = 0; index < config_.outputs.size(); ++index) {\n"
    "            if (config_.outputs[index].outputType == \"udp-cbr\") {\n"
    "                observedCbrOutputIndex = index;\n"
    "                break;\n"
    "            }\n"
    "        }\n"
    "    }\n"
    "    const bool shareObservedCbrWithUdpOutput =\n"
    "        observedCbrOutputIndex < config_.outputs.size();\n\n"
    "    auto seed = static_cast<std::uint64_t>(\n"
    "        std::chrono::steady_clock::now().time_since_epoch().count());\n",
    "precompute observer share")

old_create = '''        if (config_.outputs[index].outputType == "udp-cbr") {
            try {
                output.cbrPacer = std::make_unique<dvbstreamer5::media::mpegts::CbrTsPacer>(
                    config_.targetBitrate, cbrPacingProfile);
            } catch (const std::exception& exception) {
                std::lock_guard<std::mutex> lock(errorMutex_);
                lastError_ = exception.what();
                running_.store(false, std::memory_order_release);
                httpQueueCondition_.notify_all();
                return;
            }
        }
'''
new_create = '''        if (config_.outputs[index].outputType == "udp-cbr") {
            try {
                if (tvStreammerSat5CbrProfile) {
                    TvSatCbrOutputWorker::Observer observer;
                    if (shareObservedCbrWithUdpOutput &&
                        index == observedCbrOutputIndex) {
                        const auto transportObserver = config_.observeTransport;
                        observer = [transportObserver](
                            const std::uint8_t* data, std::size_t size) mutable {
                            transportObserver(data, size);
                        };
                    }
                    output.tvSatCbrWorker = std::make_unique<TvSatCbrOutputWorker>(
                        output.socket,
                        index,
                        config_.targetBitrate,
                        &outputBytes_,
                        &running_,
                        std::move(observer),
                        [this](const std::string& message) {
                            {
                                std::lock_guard<std::mutex> lock(errorMutex_);
                                lastError_ = message;
                            }
                            running_.store(false, std::memory_order_release);
                            httpQueueCondition_.notify_all();
                        });
                } else {
                    output.cbrPacer =
                        std::make_unique<dvbstreamer5::media::mpegts::CbrTsPacer>(
                            config_.targetBitrate, cbrPacingProfile);
                }
            } catch (const std::exception& exception) {
                std::lock_guard<std::mutex> lock(errorMutex_);
                lastError_ = exception.what();
                running_.store(false, std::memory_order_release);
                httpQueueCondition_.notify_all();
                return;
            }
        }
'''
text = replace_once(text, old_create, new_create, "construct dedicated SAT5 sender")

old_share = '''    // V10.8.156: when a native UDP-CBR output already exists, reuse its
    // paced datagrams for observeTransport instead of running a second
    // independent CBR pacer/thread for the same stream. This keeps public
    // HTTP/SRT/HLS/RTSP/RTMP/statistics on the exact emitted CBR timeline
    // and removes one high-frequency pacing thread per UDP-CBR channel.
    std::size_t observedCbrOutputIndex = outputs.size();
    if (config_.paceObservedTransport && config_.observeTransport) {
        for (std::size_t index = 0; index < outputs.size(); ++index) {
            if (outputs[index].cbrPacer) {
                observedCbrOutputIndex = index;
                break;
            }
        }
    }
    const bool shareObservedCbrWithUdpOutput =
        observedCbrOutputIndex < outputs.size();
    if (shareObservedCbrWithUdpOutput) {
        std::cerr << "NATIVE OBSERVED CBR share output_index="
                  << observedCbrOutputIndex
                  << " target_kbps=" << (config_.targetBitrate / 1000ULL)
                  << std::endl;
    }
'''
new_share = '''    // Reuse the already-shaped first UDP-CBR output for observer fan-out.
    // For the SAT5 profile the dedicated sender publishes after the UDP send;
    // Standard CBR retains the V10.8.156 in-loop publication below.
    if (shareObservedCbrWithUdpOutput) {
        std::cerr << "NATIVE OBSERVED CBR share output_index="
                  << observedCbrOutputIndex
                  << " target_kbps=" << (config_.targetBitrate / 1000ULL)
                  << " sender="
                  << (tvStreammerSat5CbrProfile ? "tvstreammersat5-dedicated" : "native")
                  << std::endl;
    }
'''
text = replace_once(text, old_share, new_share, "replace observer share discovery")

old_enqueue = '''            for (auto& output : outputs) {
                if (output.cbrPacer) {
                    for (const auto& packet : packets) {
                        if (!output.cbrPacer->enqueue(packet)) {
                            error = "native UDP CBR input exceeded the bounded 2 MiB pacing queue";
                            break;
                        }
                    }
                    if (!error.empty()) {
                        std::lock_guard<std::mutex> lock(errorMutex_);
                        lastError_ = error;
                        break;
                    }
                } else if (!flushPackets(
'''
new_enqueue = '''            for (auto& output : outputs) {
                if (output.tvSatCbrWorker) {
                    if (!output.tvSatCbrWorker->enqueue(packets)) {
                        if (running_.load(std::memory_order_acquire)) {
                            error = "TVStreammerSAT5 UDP CBR input stopped while queuing transport";
                            std::lock_guard<std::mutex> lock(errorMutex_);
                            lastError_ = error;
                        }
                        break;
                    }
                } else if (output.cbrPacer) {
                    for (const auto& packet : packets) {
                        if (!output.cbrPacer->enqueue(packet)) {
                            error = "native UDP CBR input exceeded the bounded 2 MiB pacing queue";
                            break;
                        }
                    }
                    if (!error.empty()) {
                        std::lock_guard<std::mutex> lock(errorMutex_);
                        lastError_ = error;
                        break;
                    }
                } else if (!flushPackets(
'''
text = replace_once(text, old_enqueue, new_enqueue, "enqueue into dedicated SAT5 sender")

# Stop dedicated sender threads before tearing down observer/output state.
text = replace_once(
    text,
    "    {\n"
    "        std::lock_guard<std::mutex> lock(observedCbrMutex);\n"
    "        observedCbrStop = true;\n"
    "    }\n",
    "    for (auto& output : outputs) {\n"
    "        if (output.tvSatCbrWorker) output.tvSatCbrWorker->stop();\n"
    "    }\n\n"
    "    {\n"
    "        std::lock_guard<std::mutex> lock(observedCbrMutex);\n"
    "        observedCbrStop = true;\n"
    "    }\n",
    "stop dedicated SAT5 senders")

path.write_text(text)

# --- version ---
path = Path("src/AppVersion.h")
text = path.read_text()
text = replace_once(text, 'kProgramVersion = "10.8.158"',
                    'kProgramVersion = "10.8.159"', "version bump")
path.write_text(text)

print("V10.8.159 SAT5 CBR patch applied")
