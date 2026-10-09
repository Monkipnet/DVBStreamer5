#pragma once

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
