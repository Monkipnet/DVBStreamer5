#pragma once

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
#include <stdexcept>
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
