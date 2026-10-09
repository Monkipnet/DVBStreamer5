#!/usr/bin/env python3
from pathlib import Path
import subprocess

BASE = "5afa3efc5f4e4102c99739227a9c442c9d3b401a"


def git_show(path: str) -> str:
    return subprocess.check_output(["git", "show", f"{BASE}:{path}"], text=True)


cbr_h = r'''#pragma once

#include "media/TransportStream.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>

namespace dvbstreamer5::media::mpegts {

// WISI Chameleon interoperable MPEG-over-UDP layout: 7 * 188 = 1316 bytes.
inline constexpr std::size_t kPacketsPerCbrDatagram = 7;
using CbrDatagram = std::array<Packet, kPacketsPerCbrDatagram>;

// Fixed-rate WISI transport shaper. It deliberately does not estimate or tune
// the requested transport bitrate. Source NULL packets are discarded and the
// shaper regenerates exactly the NULL budget required by the configured CBR.
// PCR/PTS/DTS bytes in useful source packets are preserved byte-for-byte.
class CbrTsPacer {
public:
    explicit CbrTsPacer(std::uint64_t targetBitrate);

    bool canEnqueue(const Packet& packet) const noexcept;
    bool enqueue(const Packet& packet);
    bool nextDatagram(
        std::chrono::steady_clock::time_point now,
        CbrDatagram& datagram);

    bool started() const noexcept;
    std::chrono::steady_clock::time_point nextDeadline() const noexcept;
    std::uint64_t targetBitrate() const noexcept;
    std::size_t queuedPackets() const noexcept;
    std::size_t queuedBytes() const noexcept;
    static constexpr std::size_t maximumQueuedBytes() noexcept {
        return kMaximumQueuedBytes;
    }

private:
    void advanceDeadline() noexcept;
    void makeNullPacket(Packet& packet) noexcept;
    static bool isNullPacket(const Packet& packet) noexcept;

    // Large enough for ordinary HTTP/SRT/HLS delivery bursts without turning
    // the shaper into a multi-second SAT5-style playout reservoir.
    static constexpr std::size_t kMaximumQueuedBytes = 8 * 1024 * 1024;
    static constexpr std::uint64_t kMinimumBitrate = 100000;
    static constexpr std::uint64_t kMaximumBitrate = 200000000;

    std::uint64_t targetBitrate_;
    std::deque<Packet> queuedPackets_;
    std::chrono::steady_clock::time_point nextDeadline_ {};
    std::uint64_t pacingRemainder_ = 0;
    std::uint8_t nullContinuityCounter_ = 0;
    bool started_ = false;
};

} // namespace dvbstreamer5::media::mpegts
'''

cbr_cpp = r'''#include "media/CbrTsPacer.h"

#include <algorithm>
#include <stdexcept>

namespace dvbstreamer5::media::mpegts {
namespace {

constexpr std::uint64_t kNanosecondsPerSecond = 1000000000ULL;
constexpr std::uint64_t kDatagramBits =
    kPacketsPerCbrDatagram * kPacketSize * 8ULL;

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

    // WISI output owns the stuffing domain. Never preserve source NULL packets:
    // they contain no service payload and would make bursty network input look
    // artificially busy. Recreate PID 0x1fff only at the exact transport slots
    // left empty by real packets.
    if (isNullPacket(packet)) return true;
    if (!canEnqueue(packet)) return false;

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
    if (!started_ || now < nextDeadline_) return false;

    // A late scheduler wakeup must never be repaid by a burst of back-to-back
    // 1316-byte UDP packets. WISI is sensitive to that instantaneous bitrate.
    // Keep sub-period error, but re-anchor after one full missed datagram slot.
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

bool CbrTsPacer::started() const noexcept { return started_; }

std::chrono::steady_clock::time_point CbrTsPacer::nextDeadline() const noexcept {
    return nextDeadline_;
}

std::uint64_t CbrTsPacer::targetBitrate() const noexcept { return targetBitrate_; }

std::size_t CbrTsPacer::queuedPackets() const noexcept { return queuedPackets_.size(); }

std::size_t CbrTsPacer::queuedBytes() const noexcept {
    return queuedPackets_.size() * kPacketSize;
}

bool CbrTsPacer::isNullPacket(const Packet& packet) noexcept {
    const std::uint16_t pid = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(packet[1] & 0x1fU) << 8) | packet[2]);
    return pid == kNullPid;
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
'''

worker_h = r'''#pragma once

#include "media/CbrTsPacer.h"
#include "media/UdpSocket.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

#ifndef _WIN32
#include <cerrno>
#include <ctime>
#endif

namespace dvbstreamer5::media::network {

// Dedicated WISI UDP-CBR sender. The relay/input thread only feeds useful TS;
// this worker owns the physical output clock. That prevents HTTP/SRT/HLS input
// bursts, remap/CA work and observer delivery from moving UDP send deadlines.
class WisiCbrOutputWorker {
public:
    using Observer = std::function<void(const std::uint8_t*, std::size_t)>;
    using ErrorCallback = std::function<void(const std::string&)>;

    WisiCbrOutputWorker(
        UdpSocket* socket,
        std::size_t outputIndex,
        std::uint64_t targetBitrate,
        std::atomic<std::uint64_t>* outputBytes,
        std::atomic<bool>* relayRunning,
        Observer observer,
        ErrorCallback errorCallback)
        : socket_(socket),
          outputIndex_(outputIndex),
          pacer_(targetBitrate),
          outputBytes_(outputBytes),
          relayRunning_(relayRunning),
          observer_(std::move(observer)),
          errorCallback_(std::move(errorCallback)) {
        if (!socket_ || !outputBytes_ || !relayRunning_) {
            throw std::invalid_argument("WISI CBR worker requires valid relay objects");
        }
        if (observer_) {
            observerThread_ = std::thread(&WisiCbrOutputWorker::observerLoop, this);
        }
        senderThread_ = std::thread(&WisiCbrOutputWorker::senderLoop, this);
    }

    ~WisiCbrOutputWorker() { stop(); }

    WisiCbrOutputWorker(const WisiCbrOutputWorker&) = delete;
    WisiCbrOutputWorker& operator=(const WisiCbrOutputWorker&) = delete;

    bool enqueue(const mpegts::Packet& packet) {
        if (packet[0] != mpegts::kSyncByte) return false;
        std::unique_lock<std::mutex> lock(mutex_);
        bool backpressureLogged = false;
        while (!stopping_ && relayRunning_->load(std::memory_order_acquire) &&
               !pacer_.canEnqueue(packet)) {
            if (!backpressureLogged) {
                backpressureLogged = true;
                std::cerr << "WISI CBR BACKPRESSURE index=" << outputIndex_
                          << " queued_kb=" << (pacer_.queuedBytes() / 1024U)
                          << " limit_kb=" << (mpegts::CbrTsPacer::maximumQueuedBytes() / 1024U)
                          << " target_kbps=" << (pacer_.targetBitrate() / 1000ULL)
                          << std::endl;
            }
            queueSpace_.wait_for(lock, std::chrono::milliseconds(20));
        }
        if (stopping_ || !relayRunning_->load(std::memory_order_acquire)) return false;

        const bool wasStarted = pacer_.started();
        if (!pacer_.enqueue(packet)) return false;
        if (!wasStarted && pacer_.started()) {
            firstPacketAt_ = std::chrono::steady_clock::now();
            senderWake_.notify_one();
        }
        const std::size_t queued = pacer_.queuedPackets();
        if (queued > queuePeakPackets_) queuePeakPackets_ = queued;
        return true;
    }

    bool started() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return pacer_.started();
    }

    std::chrono::steady_clock::time_point nextDeadline() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return pacer_.nextDeadline();
    }

    std::size_t queuedPackets() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return pacer_.queuedPackets();
    }

    void stop() noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        senderWake_.notify_all();
        queueSpace_.notify_all();
        {
            std::lock_guard<std::mutex> lock(observerMutex_);
            observerStopping_ = true;
        }
        observerReady_.notify_all();

        if (senderThread_.joinable() && senderThread_.get_id() != std::this_thread::get_id()) {
            senderThread_.join();
        }
        if (observerThread_.joinable() && observerThread_.get_id() != std::this_thread::get_id()) {
            observerThread_.join();
        }
    }

private:
    static constexpr auto kStartupBuffer = std::chrono::milliseconds(400);
    static constexpr std::size_t kObserverQueueDatagrams = 4096;

#ifndef _WIN32
    static std::uint64_t monotonicNanoseconds() noexcept {
        timespec now {};
        if (::clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
        return static_cast<std::uint64_t>(now.tv_sec) * 1000000000ULL +
            static_cast<std::uint64_t>(now.tv_nsec);
    }

    static void sleepUntil(std::chrono::steady_clock::time_point deadline) noexcept {
        const auto nowSteady = std::chrono::steady_clock::now();
        if (deadline <= nowSteady) return;
        const auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(
            deadline - nowSteady).count();
        const std::uint64_t monoNow = monotonicNanoseconds();
        if (monoNow == 0 || remaining <= 0) return;
        const std::uint64_t target = monoNow + static_cast<std::uint64_t>(remaining);
        timespec ts {};
        ts.tv_sec = static_cast<time_t>(target / 1000000000ULL);
        ts.tv_nsec = static_cast<long>(target % 1000000000ULL);
        int result = 0;
        do {
            result = ::clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr);
        } while (result == EINTR);
    }
#else
    static void sleepUntil(std::chrono::steady_clock::time_point deadline) noexcept {
        std::this_thread::sleep_until(deadline);
    }
#endif

    void fail(const std::string& message) noexcept {
        relayRunning_->store(false, std::memory_order_release);
        if (errorCallback_) {
            try { errorCallback_(message); } catch (...) {}
        }
        senderWake_.notify_all();
        queueSpace_.notify_all();
        observerReady_.notify_all();
    }

    void queueObserverDatagram(const std::uint8_t* data, std::size_t size) noexcept {
        if (!observer_ || !data || size != kDatagramBytes) return;
        std::array<std::uint8_t, kDatagramBytes> copy {};
        std::memcpy(copy.data(), data, size);
        {
            std::lock_guard<std::mutex> lock(observerMutex_);
            if (observerQueue_.size() >= kObserverQueueDatagrams) {
                observerQueue_.pop_front();
                ++observerDrops_;
            }
            observerQueue_.push_back(std::move(copy));
        }
        observerReady_.notify_one();
    }

    void observerLoop() noexcept {
        try {
            while (true) {
                std::array<std::uint8_t, kDatagramBytes> datagram {};
                {
                    std::unique_lock<std::mutex> lock(observerMutex_);
                    observerReady_.wait(lock, [this] {
                        return observerStopping_ || !observerQueue_.empty();
                    });
                    if (observerStopping_ && observerQueue_.empty()) break;
                    datagram = std::move(observerQueue_.front());
                    observerQueue_.pop_front();
                }
                observer_(datagram.data(), datagram.size());
            }
        } catch (const std::exception& ex) {
            fail(std::string("WISI CBR observer failed: ") + ex.what());
        } catch (...) {
            fail("WISI CBR observer failed");
        }
    }

    void senderLoop() noexcept {
        try {
            bool startupComplete = false;
            std::unique_lock<std::mutex> lock(mutex_);
            while (!stopping_ && relayRunning_->load(std::memory_order_acquire)) {
                if (!pacer_.started()) {
                    senderWake_.wait_for(lock, std::chrono::milliseconds(20));
                    continue;
                }

                if (!startupComplete) {
                    const auto startAt = firstPacketAt_ + kStartupBuffer;
                    if (std::chrono::steady_clock::now() < startAt) {
                        senderWake_.wait_until(lock, startAt, [this] {
                            return stopping_ || !relayRunning_->load(std::memory_order_acquire);
                        });
                        if (stopping_ || !relayRunning_->load(std::memory_order_acquire)) break;
                    }
                    startupComplete = true;
                }

                const auto deadline = pacer_.nextDeadline();
                lock.unlock();
                sleepUntil(deadline);
                const auto wokeAt = std::chrono::steady_clock::now();
                lock.lock();
                if (stopping_ || !relayRunning_->load(std::memory_order_acquire)) break;

                mpegts::CbrDatagram datagram {};
                const std::size_t queuedBefore = pacer_.queuedPackets();
                if (!pacer_.nextDatagram(wokeAt, datagram)) continue;
                const std::size_t queuedAfter = pacer_.queuedPackets();
                if (queuedAfter < queuedBefore) queueSpace_.notify_all();
                const std::uint64_t targetBitrate = pacer_.targetBitrate();

                const std::uint64_t periodNs =
                    (kDatagramBytes * 8ULL * 1000000000ULL) / targetBitrate;
                if (wokeAt > deadline) {
                    const auto lateNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        wokeAt - deadline).count();
                    const std::uint64_t reportThreshold =
                        (std::max<std::uint64_t>)(periodNs * 4ULL, 20000000ULL);
                    if (lateNs >= 0 && static_cast<std::uint64_t>(lateNs) >= reportThreshold) {
                        ++lateEvents_;
                        lateMaxUs_ = (std::max<std::uint64_t>)(
                            lateMaxUs_, static_cast<std::uint64_t>(lateNs) / 1000ULL);
                        const auto now = std::chrono::steady_clock::now();
                        if (lastLateLog_ == std::chrono::steady_clock::time_point{} ||
                            now - lastLateLog_ >= std::chrono::seconds(5)) {
                            lastLateLog_ = now;
                            std::cerr << "WISI CBR LATE index=" << outputIndex_
                                      << " late_events=" << lateEvents_
                                      << " max_late_us=" << lateMaxUs_
                                      << " queued_kb=" << (pacer_.queuedBytes() / 1024U)
                                      << " target_kbps=" << (targetBitrate / 1000ULL)
                                      << std::endl;
                        }
                    }
                }

                const auto* bytes = reinterpret_cast<const std::uint8_t*>(datagram.data());
                lock.unlock();
                std::string sendError;
                if (!socket_->send(bytes, kDatagramBytes, sendError)) {
                    fail(sendError.empty()
                        ? "WISI UDP CBR output send failed"
                        : "WISI UDP CBR output send failed: " + sendError);
                    lock.lock();
                    stopping_ = true;
                    break;
                }
                outputBytes_->fetch_add(kDatagramBytes, std::memory_order_relaxed);
                queueObserverDatagram(bytes, kDatagramBytes);

                if (!firstSendLogged_) {
                    firstSendLogged_ = true;
                    std::cerr << "WISI CBR start index=" << outputIndex_
                              << " target_kbps=" << (targetBitrate / 1000ULL)
                              << " datagram_bytes=" << kDatagramBytes
                              << " startup_buffer_ms=400"
                              << " queue_kb=" << (queuedAfter * mpegts::kPacketSize / 1024U)
                              << " source_pcr=preserved"
                              << " pts_dts=preserved"
                              << " auto_tune=off"
                              << " catchup_burst=off"
                              << " sender_clock=CLOCK_MONOTONIC_ABSTIME"
                              << std::endl;
                    std::cerr << "NATIVE UDP OUTPUT first_send index=" << outputIndex_
                              << " type=udp-cbr profile=wisi-fixed-cbr"
                              << " sender_thread=dedicated"
                              << std::endl;
                }
                lock.lock();
            }
        } catch (const std::exception& ex) {
            fail(std::string("WISI CBR sender failed: ") + ex.what());
        } catch (...) {
            fail("WISI CBR sender failed");
        }
    }

    static constexpr std::size_t kDatagramBytes =
        mpegts::kPacketsPerCbrDatagram * mpegts::kPacketSize;

    UdpSocket* socket_ = nullptr;
    std::size_t outputIndex_ = 0;
    mutable std::mutex mutex_;
    mpegts::CbrTsPacer pacer_;
    std::atomic<std::uint64_t>* outputBytes_ = nullptr;
    std::atomic<bool>* relayRunning_ = nullptr;
    Observer observer_;
    ErrorCallback errorCallback_;
    std::condition_variable senderWake_;
    std::condition_variable queueSpace_;
    bool stopping_ = false;
    bool firstSendLogged_ = false;
    std::size_t queuePeakPackets_ = 0;
    std::chrono::steady_clock::time_point firstPacketAt_ {};
    std::chrono::steady_clock::time_point lastLateLog_ {};
    std::uint64_t lateEvents_ = 0;
    std::uint64_t lateMaxUs_ = 0;
    std::thread senderThread_;

    std::mutex observerMutex_;
    std::condition_variable observerReady_;
    std::deque<std::array<std::uint8_t, kDatagramBytes>> observerQueue_;
    bool observerStopping_ = false;
    std::uint64_t observerDrops_ = 0;
    std::thread observerThread_;
};

} // namespace dvbstreamer5::media::network
'''

Path("src/media/CbrTsPacer.h").write_text(cbr_h)
Path("src/media/CbrTsPacer.cpp").write_text(cbr_cpp)
Path("src/media/WisiCbrOutputWorker.h").write_text(worker_h)

# Restore relay structure from the last pre-SAT5 CBR baseline, then replace only
# UDP-CBR output pacing with the dedicated WISI worker.
relay = git_show("src/media/NativeUdpRelay.cpp")
relay = relay.replace(
    '#include "media/CbrTsPacer.h"\n',
    '#include "media/CbrTsPacer.h"\n#include "media/WisiCbrOutputWorker.h"\n',
    1)
relay = relay.replace(
    '        std::unique_ptr<dvbstreamer5::media::mpegts::CbrTsPacer> cbrPacer;\n',
    '        std::unique_ptr<dvbstreamer5::media::network::WisiCbrOutputWorker> cbrWorker;\n',
    1)

anchor = '    std::vector<OutputWorker> outputs;\n    outputs.reserve(config_.outputs.size());\n'
if anchor not in relay:
    raise SystemExit('outputs reserve anchor not found')
insert = anchor + '''    std::size_t observedCbrOutputIndex = config_.outputs.size();
    if (config_.paceObservedTransport && config_.observeTransport) {
        for (std::size_t index = 0; index < config_.outputs.size(); ++index) {
            if (config_.outputs[index].outputType == "udp-cbr") {
                observedCbrOutputIndex = index;
                break;
            }
        }
    }
'''
relay = relay.replace(anchor, insert, 1)

old_ctor = '''        if (config_.outputs[index].outputType == "udp-cbr") {
            try {
                output.cbrPacer = std::make_unique<dvbstreamer5::media::mpegts::CbrTsPacer>(
                    config_.targetBitrate);
            } catch (const std::exception& exception) {
                std::lock_guard<std::mutex> lock(errorMutex_);
                lastError_ = exception.what();
                running_.store(false, std::memory_order_release);
                httpQueueCondition_.notify_all();
                return;
            }
        }
'''
new_ctor = '''        if (config_.outputs[index].outputType == "udp-cbr") {
            try {
                dvbstreamer5::media::network::WisiCbrOutputWorker::Observer observer;
                if (index == observedCbrOutputIndex) observer = config_.observeTransport;
                output.cbrWorker =
                    std::make_unique<dvbstreamer5::media::network::WisiCbrOutputWorker>(
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
            } catch (const std::exception& exception) {
                std::lock_guard<std::mutex> lock(errorMutex_);
                lastError_ = exception.what();
                running_.store(false, std::memory_order_release);
                httpQueueCondition_.notify_all();
                return;
            }
        }
'''
if old_ctor not in relay:
    raise SystemExit('CBR constructor block not found')
relay = relay.replace(old_ctor, new_ctor, 1)

# All output-worker queue/deadline checks now refer to the dedicated worker.
relay = relay.replace('output.cbrPacer', 'output.cbrWorker')
relay = relay.replace('outputs[index].cbrPacer', 'outputs[index].cbrWorker')

# Replace the old post-construction scan: observedCbrOutputIndex was selected
# before worker creation so the first CBR worker can own an async observer tap.
start = relay.index('    // V10.8.156: when a native UDP-CBR output already exists')
end = relay.index('    // V10.8.105: observeTransport feeds SRT/HTTP/HLS/RTSP/RTMP.', start)
replacement = '''    // WISI CBR: the first UDP-CBR worker publishes its already-shaped
    // datagrams to observeTransport asynchronously. Observer work can never
    // block the physical WISI UDP sender clock.
    const bool shareObservedCbrWithUdpOutput =
        observedCbrOutputIndex < outputs.size();
    if (shareObservedCbrWithUdpOutput) {
        std::cerr << "NATIVE OBSERVED CBR share output_index="
                  << observedCbrOutputIndex
                  << " target_kbps=" << (config_.targetBitrate / 1000ULL)
                  << " profile=wisi-fixed-cbr async_observer=1"
                  << std::endl;
    }

'''
relay = relay[:start] + replacement + relay[end:]

# The dedicated worker owns UDP-CBR deadlines and socket sends; remove the old
# relay-loop drain that could bunch several overdue datagrams after input work.
marker = relay.index('            // Drain every datagram whose pacing deadline has already arrived.')
block_start = relay.rfind('        for (auto& output : outputs) {', 0, marker)
cleanup = '\n    {\n        std::lock_guard<std::mutex> lock(observedCbrMutex);'
block_end = relay.index(cleanup, marker)
relay = relay[:block_start] + '''        // UDP-CBR is drained by WisiCbrOutputWorker on its own absolute
        // CLOCK_MONOTONIC sender thread. Never send CBR datagrams here.
''' + relay[block_end:]

Path("src/media/NativeUdpRelay.cpp").write_text(relay)

# Remove the experimental SAT5 sender from the new line of development.
sat_worker = Path("src/media/TvSatCbrOutputWorker.h")
if sat_worker.exists():
    sat_worker.unlink()

app = Path("src/AppVersion.h")
version = app.read_text()
if 'kProgramVersion = "10.8.163"' not in version:
    raise SystemExit('unexpected AppVersion base')
app.write_text(version.replace('kProgramVersion = "10.8.163"', 'kProgramVersion = "10.8.164"'))

# One-shot transformer/workflow must not remain in the product tree.
Path("tools/apply_wisi_cbr_v164.py").unlink(missing_ok=True)
Path(".github/workflows/wisi-cbr-v164.yml").unlink(missing_ok=True)
