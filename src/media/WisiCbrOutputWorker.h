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
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#ifndef _WIN32
#include <cerrno>
#include <ctime>
#include <pthread.h>
#include <sched.h>
#include <sys/prctl.h>
#endif

namespace dvbstreamer5::media::network {

// V172 WISI-compatible UDP-CBR output path.
//
// The input/remap/CA thread only appends useful TS packets to an ingress FIFO.
// The sender thread never shares the MPEG-TS pacer lock with the producer. It
// owns a dedicated CLOCK_MONOTONIC transport clock, creates exactly 7x188-byte
// datagrams, fills unused TS slots with PID 0x1fff and sends one datagram per
// transport period. This isolation is deliberate: DVB, HTTP, SRT and HLS all
// use the same physical UDP-CBR clock and therefore have identical output
// timing behaviour.
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
          startupBuffer_(std::chrono::milliseconds(
              mpegts::CbrTsPacer::startupBufferMilliseconds(targetBitrate))),
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

        // The output remultiplexer owns all NULL stuffing. Do not even put
        // source NULL packets into the producer FIFO; this keeps the ingress
        // lock short for satellite CBR sources that already contain many NULLs.
        if (mpegts::CbrTsPacer::sourcePacketIsNull(packet)) return true;

        const auto now = std::chrono::steady_clock::now();
        std::unique_lock<std::mutex> lock(ingressMutex_);
        while (!stopping_.load(std::memory_order_acquire) &&
               relayRunning_->load(std::memory_order_acquire) &&
               bufferedPackets_.load(std::memory_order_relaxed) >= kMaximumBufferedPackets) {
            if (!backpressureLogged_) {
                backpressureLogged_ = true;
                std::cerr << "WISI CBR BACKPRESSURE index=" << outputIndex_
                          << " buffered_kb="
                          << (bufferedPackets_.load(std::memory_order_relaxed) *
                              mpegts::kPacketSize / 1024U)
                          << " limit_kb="
                          << (mpegts::CbrTsPacer::maximumQueuedBytes() / 1024U)
                          << " target_kbps=" << (pacer_.targetBitrate() / 1000ULL)
                          << std::endl;
            }
            queueSpace_.wait_for(lock, std::chrono::milliseconds(20));
        }
        if (stopping_.load(std::memory_order_acquire) ||
            !relayRunning_->load(std::memory_order_acquire)) {
            return false;
        }

        ingressQueue_.push_back({packet, now});
        const std::size_t buffered =
            bufferedPackets_.fetch_add(1U, std::memory_order_relaxed) + 1U;
        if (buffered > queuePeakPackets_) queuePeakPackets_ = buffered;
        if (!inputStarted_.load(std::memory_order_relaxed)) {
            firstPacketAt_ = now;
            inputStarted_.store(true, std::memory_order_release);
        }
        lock.unlock();
        ingressReady_.notify_one();
        return true;
    }

    bool started() const noexcept {
        return inputStarted_.load(std::memory_order_acquire);
    }

    std::chrono::steady_clock::time_point nextDeadline() const noexcept {
        const std::int64_t value = nextDeadlineSteadyNs_.load(std::memory_order_relaxed);
        return std::chrono::steady_clock::time_point(std::chrono::nanoseconds(value));
    }

    std::size_t queuedPackets() const noexcept {
        return bufferedPackets_.load(std::memory_order_relaxed);
    }

    void stop() noexcept {
        const bool alreadyStopping =
            stopping_.exchange(true, std::memory_order_acq_rel);
        ingressReady_.notify_all();
        queueSpace_.notify_all();
        {
            std::lock_guard<std::mutex> lock(observerMutex_);
            observerStopping_ = true;
        }
        observerReady_.notify_all();

        if (!alreadyStopping && senderThread_.joinable() &&
            senderThread_.get_id() != std::this_thread::get_id()) {
            senderThread_.join();
        } else if (senderThread_.joinable() &&
                   senderThread_.get_id() != std::this_thread::get_id()) {
            senderThread_.join();
        }
        if (observerThread_.joinable() &&
            observerThread_.get_id() != std::this_thread::get_id()) {
            observerThread_.join();
        }
    }

private:
    struct IngressPacket {
        mpegts::Packet packet {};
        std::chrono::steady_clock::time_point arrival {};
    };

    static constexpr std::size_t kObserverQueueDatagrams = 4096;
    static constexpr std::size_t kDatagramBytes =
        mpegts::kPacketsPerCbrDatagram * mpegts::kPacketSize;
    static constexpr std::size_t kMaximumBufferedPackets =
        mpegts::CbrTsPacer::maximumQueuedBytes() / mpegts::kPacketSize;
    static constexpr std::uint64_t kNanosecondsPerSecond = 1000000000ULL;
    static constexpr std::uint64_t kSpinGuardNanoseconds = 30000ULL; // 30 us
    static constexpr std::uint64_t kPhaseCorrectionDivisor = 400ULL; // 0.25%
    static constexpr std::uint64_t kClockLogNanoseconds = 5ULL * kNanosecondsPerSecond;

    void configureSenderScheduling() noexcept {
#ifndef _WIN32
        // Minimise timer coalescing first. This matters more than priority for
        // 2-4 ms UDP periods and is safe even when SCHED_FIFO is unavailable.
        (void)::prctl(PR_SET_TIMERSLACK, 1UL, 0UL, 0UL, 0UL);

        sched_param parameters {};
        const int minimum = ::sched_get_priority_min(SCHED_FIFO);
        const int maximum = ::sched_get_priority_max(SCHED_FIFO);
        parameters.sched_priority =
            minimum >= 0 && maximum >= 0 ? (std::min)(maximum, minimum + 1) : 1;
        const int result = ::pthread_setschedparam(
            ::pthread_self(), SCHED_FIFO, &parameters);
        if (result == 0) {
            std::cerr << "WISI CBR sender scheduling index=" << outputIndex_
                      << " policy=SCHED_FIFO priority=" << parameters.sched_priority
                      << " timerslack_ns=1" << std::endl;
        } else {
            std::cerr << "WISI CBR sender scheduling index=" << outputIndex_
                      << " policy=CFS fallback_error=" << result
                      << " timerslack_ns=1" << std::endl;
        }
#endif
    }

    static std::uint64_t monotonicNanoseconds() noexcept {
#ifndef _WIN32
        timespec now {};
        if (::clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
        return static_cast<std::uint64_t>(now.tv_sec) * kNanosecondsPerSecond +
            static_cast<std::uint64_t>(now.tv_nsec);
#else
        const auto value = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        return value > 0 ? static_cast<std::uint64_t>(value) : 0ULL;
#endif
    }

    static void sleepUntilMonotonic(std::uint64_t deadlineNs) noexcept {
        if (deadlineNs == 0) return;
#ifndef _WIN32
        std::uint64_t now = monotonicNanoseconds();
        if (now >= deadlineNs) return;

        // Sleep most of the period in the kernel, then use a very short final
        // spin. This avoids the 100-300 us CFS wakeup error that Chameleon can
        // expose as a transport-rate step, without burning a CPU for the whole
        // 2-4 ms datagram period.
        const std::uint64_t sleepTarget = deadlineNs > kSpinGuardNanoseconds
            ? deadlineNs - kSpinGuardNanoseconds
            : deadlineNs;
        if (sleepTarget > now) {
            timespec ts {};
            ts.tv_sec = static_cast<time_t>(sleepTarget / kNanosecondsPerSecond);
            ts.tv_nsec = static_cast<long>(sleepTarget % kNanosecondsPerSecond);
            int result = 0;
            do {
                result = ::clock_nanosleep(
                    CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr);
            } while (result == EINTR);
        }
        while ((now = monotonicNanoseconds()) < deadlineNs) {
            // Deliberately empty: at most ~30 us on the production Linux path.
        }
#else
        const auto deadline = std::chrono::steady_clock::time_point(
            std::chrono::nanoseconds(deadlineNs));
        std::this_thread::sleep_until(deadline);
#endif
    }

    std::uint64_t nextPeriodNanoseconds() noexcept {
        const std::uint64_t target = pacer_.targetBitrate();
        const std::uint64_t numerator =
            kDatagramBytes * 8ULL * kNanosecondsPerSecond;
        std::uint64_t nanoseconds = numerator / target;
        periodRemainder_ += numerator % target;
        if (periodRemainder_ >= target) {
            nanoseconds += periodRemainder_ / target;
            periodRemainder_ %= target;
        }
        return (std::max<std::uint64_t>)(nanoseconds, 1ULL);
    }

    bool drainIngress() noexcept {
        std::deque<IngressPacket> pending;
        {
            std::lock_guard<std::mutex> lock(ingressMutex_);
            pending.swap(ingressQueue_);
        }
        for (auto& item : pending) {
            if (!pacer_.enqueueAt(item.packet, item.arrival)) {
                fail("WISI CBR internal remux queue overflow; increase configured CBR bitrate");
                return false;
            }
        }
        return true;
    }

    void fail(const std::string& message) noexcept {
        relayRunning_->store(false, std::memory_order_release);
        stopping_.store(true, std::memory_order_release);
        if (errorCallback_) {
            try { errorCallback_(message); } catch (...) {}
        }
        ingressReady_.notify_all();
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

    void updateClockDiagnostics(
        std::uint64_t sendNs,
        std::uint64_t gapNs,
        std::uint64_t lateNs) noexcept {
        // V173: the 5-second CLOCK line was temporary bring-up telemetry. Keep
        // fault/startup logs, but remove this hot-path diagnostic noise entirely.
        (void)sendNs;
        (void)gapNs;
        (void)lateNs;
        return;

        if (clockWindowStartNs_ == 0) {
            clockWindowStartNs_ = sendNs;
            clockWindowDatagrams_ = 0;
            clockMinGapNs_ = (std::numeric_limits<std::uint64_t>::max)();
            clockMaxGapNs_ = 0;
            clockMaxLateNs_ = 0;
        }
        ++clockWindowDatagrams_;
        if (gapNs > 0) {
            clockMinGapNs_ = (std::min)(clockMinGapNs_, gapNs);
            clockMaxGapNs_ = (std::max)(clockMaxGapNs_, gapNs);
        }
        clockMaxLateNs_ = (std::max)(clockMaxLateNs_, lateNs);

        const std::uint64_t elapsed = sendNs >= clockWindowStartNs_
            ? sendNs - clockWindowStartNs_
            : 0ULL;
        if (elapsed < kClockLogNanoseconds) return;

        const unsigned __int128 bits =
            static_cast<unsigned __int128>(clockWindowDatagrams_) *
            static_cast<unsigned __int128>(kDatagramBytes * 8ULL) *
            static_cast<unsigned __int128>(kNanosecondsPerSecond);
        const std::uint64_t actualBps = elapsed > 0
            ? static_cast<std::uint64_t>(bits / elapsed)
            : 0ULL;
        const std::uint64_t minGapUs =
            clockMinGapNs_ == (std::numeric_limits<std::uint64_t>::max)()
                ? 0ULL : clockMinGapNs_ / 1000ULL;

        std::cerr << "WISI CBR CLOCK index=" << outputIndex_
                  << " target_kbps=" << (pacer_.targetBitrate() / 1000ULL)
                  << " actual_kbps=" << (actualBps / 1000ULL)
                  << " min_gap_us=" << minGapUs
                  << " max_gap_us=" << (clockMaxGapNs_ / 1000ULL)
                  << " max_late_us=" << (clockMaxLateNs_ / 1000ULL)
                  << " phase_debt_us=" << (phaseDebtNs_ / 1000ULL)
                  << " buffered_kb="
                  << (bufferedPackets_.load(std::memory_order_relaxed) *
                      mpegts::kPacketSize / 1024U)
                  << " source_kbps=" << (pacer_.sourcePayloadBitrate() / 1000ULL)
                  << " useful_kbps=" << (pacer_.usefulPaceBitrate() / 1000ULL)
                  << " pcr_pids=" << pacer_.pcrPidCount()
                  << " pcr_rewrites=" << pacer_.pcrRewriteCount()
                  << " source_over_target_windows=" << pacer_.sourceOverTargetWindows()
                  << std::endl;

        clockWindowStartNs_ = sendNs;
        clockWindowDatagrams_ = 0;
        clockMinGapNs_ = (std::numeric_limits<std::uint64_t>::max)();
        clockMaxGapNs_ = 0;
        clockMaxLateNs_ = 0;
    }

    void senderLoop() noexcept {
        try {
            configureSenderScheduling();

            // Wait only for the first useful TS packet. The physical transport
            // clock is not started until the configured startup reservoir has
            // been collected, so that intentional delay is never reported as
            // sender lateness.
            {
                std::unique_lock<std::mutex> lock(ingressMutex_);
                ingressReady_.wait(lock, [this] {
                    return stopping_.load(std::memory_order_acquire) ||
                        !relayRunning_->load(std::memory_order_acquire) ||
                        inputStarted_.load(std::memory_order_acquire);
                });
                if (stopping_.load(std::memory_order_acquire) ||
                    !relayRunning_->load(std::memory_order_acquire)) {
                    return;
                }
            }

            const auto startAt = firstPacketAt_ + startupBuffer_;
            while (!stopping_.load(std::memory_order_acquire) &&
                   relayRunning_->load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < startAt) {
                if (!drainIngress()) return;
                std::unique_lock<std::mutex> lock(ingressMutex_);
                ingressReady_.wait_for(lock, std::chrono::milliseconds(20));
            }
            if (stopping_.load(std::memory_order_acquire) ||
                !relayRunning_->load(std::memory_order_acquire)) {
                return;
            }
            if (!drainIngress()) return;
            if (!pacer_.started()) return;

            // Fresh transport anchor after the startup reservoir. No historical
            // input timestamp participates in the physical CBR clock.
            std::uint64_t nextDeadlineNs = monotonicNanoseconds();
            periodRemainder_ = 0;
            phaseDebtNs_ = 0;
            lastSendNs_ = 0;

            while (!stopping_.load(std::memory_order_acquire) &&
                   relayRunning_->load(std::memory_order_acquire)) {
                if (!drainIngress()) return;

                nextDeadlineSteadyNs_.store(
                    static_cast<std::int64_t>(nextDeadlineNs),
                    std::memory_order_relaxed);
                sleepUntilMonotonic(nextDeadlineNs);
                const std::uint64_t sendNs = monotonicNanoseconds();
                const std::uint64_t lateNs = sendNs > nextDeadlineNs
                    ? sendNs - nextDeadlineNs
                    : 0ULL;

                // Preserve long-term phase without ever doing a burst. Every
                // scheduling delay becomes phase debt and is repaid at at most
                // 0.25% of one datagram period per send.
                const std::uint64_t debtCap = kNanosecondsPerSecond;
                phaseDebtNs_ = (std::min)(
                    debtCap,
                    phaseDebtNs_ + (std::min)(lateNs, debtCap));

                mpegts::CbrDatagram datagram {};
                const auto nowSteady = std::chrono::steady_clock::now();
                if (!pacer_.emitDatagram(nowSteady, datagram)) {
                    // This should only be possible before the first packet; keep
                    // the transport alive as a NULL-only stream if it ever occurs.
                    for (auto& packet : datagram) {
                        packet.fill(0xff);
                        packet[0] = mpegts::kSyncByte;
                        packet[1] = 0x1f;
                        packet[2] = 0xff;
                        packet[3] = 0x10;
                    }
                }

                const std::size_t useful = pacer_.lastDatagramUsefulPackets();
                if (useful > 0) {
                    const std::size_t previous =
                        bufferedPackets_.fetch_sub(useful, std::memory_order_relaxed);
                    if (previous < useful) {
                        bufferedPackets_.store(0, std::memory_order_relaxed);
                    }
                    backpressureLogged_ = false;
                    queueSpace_.notify_all();
                } else if (pacer_.timingLocked()) {
                    ++usefulUnderflowDatagrams_;
                    const auto logNow = std::chrono::steady_clock::now();
                    if (lastUnderflowLog_ == std::chrono::steady_clock::time_point{} ||
                        logNow - lastUnderflowLog_ >= std::chrono::seconds(5)) {
                        lastUnderflowLog_ = logNow;
                        std::cerr << "WISI CBR USEFUL UNDERFLOW index=" << outputIndex_
                                  << " null_only_datagrams=" << usefulUnderflowDatagrams_
                                  << " buffered_kb="
                                  << (bufferedPackets_.load(std::memory_order_relaxed) *
                                      mpegts::kPacketSize / 1024U)
                                  << " source_kbps="
                                  << (pacer_.sourcePayloadBitrate() / 1000ULL)
                                  << " useful_kbps="
                                  << (pacer_.usefulPaceBitrate() / 1000ULL)
                                  << " target_kbps="
                                  << (pacer_.targetBitrate() / 1000ULL)
                                  << " startup_buffer_ms=" << startupBuffer_.count()
                                  << std::endl;
                    }
                }

                const auto* bytes =
                    reinterpret_cast<const std::uint8_t*>(datagram.data());
                std::string sendError;
                if (!socket_->send(bytes, kDatagramBytes, sendError)) {
                    fail(sendError.empty()
                        ? "WISI UDP CBR output send failed"
                        : "WISI UDP CBR output send failed: " + sendError);
                    return;
                }
                outputBytes_->fetch_add(kDatagramBytes, std::memory_order_relaxed);
                queueObserverDatagram(bytes, kDatagramBytes);

                const std::uint64_t gapNs = lastSendNs_ > 0 && sendNs >= lastSendNs_
                    ? sendNs - lastSendNs_
                    : 0ULL;
                lastSendNs_ = sendNs;
                updateClockDiagnostics(sendNs, gapNs, lateNs);

                if (!firstSendLogged_) {
                    firstSendLogged_ = true;
                    std::cerr << "WISI CBR start index=" << outputIndex_
                              << " target_kbps=" << (pacer_.targetBitrate() / 1000ULL)
                              << " datagram_bytes=" << kDatagramBytes
                              << " ts_packets_per_datagram=" << mpegts::kPacketsPerCbrDatagram
                              << " startup_buffer_ms=" << startupBuffer_.count()
                              << " ingress_clock=decoupled"
                              << " source_null=regenerated"
                              << " source_pcr="
                              << (pacer_.sourcePllMode() ? "preserved" : "restamped-all-pids")
                              << " pcr_schedule="
                              << (pacer_.sourcePllMode() ? "source-timeline" : "output-cbr-slot-clock")
                              << " pts_dts=preserved"
                              << " auto_tune=off"
                              << " catchup_burst=off"
                              << " phase_recovery_max_pct=0.25"
                              << " sender_clock=CLOCK_MONOTONIC_ABSTIME"
                              << std::endl;
                    std::cerr << "NATIVE UDP OUTPUT first_send index=" << outputIndex_
                              << " type=udp-cbr profile=wisi-fixed-cbr-v172"
                              << " sender_thread=dedicated"
                              << std::endl;
                }

                const std::uint64_t periodNs = nextPeriodNanoseconds();
                const std::uint64_t maximumCorrection =
                    (std::max<std::uint64_t>)(1ULL,
                        periodNs / kPhaseCorrectionDivisor);
                const std::uint64_t correction =
                    (std::min)(phaseDebtNs_, maximumCorrection);
                phaseDebtNs_ -= correction;
                nextDeadlineNs = sendNs + periodNs - correction;
            }
        } catch (const std::exception& ex) {
            fail(std::string("WISI CBR sender failed: ") + ex.what());
        } catch (...) {
            fail("WISI CBR sender failed");
        }
    }

    UdpSocket* socket_ = nullptr;
    std::size_t outputIndex_ = 0;
    mpegts::CbrTsPacer pacer_;
    const std::chrono::milliseconds startupBuffer_;
    std::atomic<std::uint64_t>* outputBytes_ = nullptr;
    std::atomic<bool>* relayRunning_ = nullptr;
    Observer observer_;
    ErrorCallback errorCallback_;

    std::atomic<bool> stopping_ {false};
    std::atomic<bool> inputStarted_ {false};
    std::atomic<std::size_t> bufferedPackets_ {0};
    std::atomic<std::int64_t> nextDeadlineSteadyNs_ {0};

    std::mutex ingressMutex_;
    std::condition_variable ingressReady_;
    std::condition_variable queueSpace_;
    std::deque<IngressPacket> ingressQueue_;
    std::chrono::steady_clock::time_point firstPacketAt_ {};
    bool backpressureLogged_ = false;
    std::size_t queuePeakPackets_ = 0;

    bool firstSendLogged_ = false;
    std::uint64_t periodRemainder_ = 0;
    std::uint64_t phaseDebtNs_ = 0;
    std::uint64_t lastSendNs_ = 0;
    std::uint64_t usefulUnderflowDatagrams_ = 0;
    std::chrono::steady_clock::time_point lastUnderflowLog_ {};

    std::uint64_t clockWindowStartNs_ = 0;
    std::uint64_t clockWindowDatagrams_ = 0;
    std::uint64_t clockMinGapNs_ = (std::numeric_limits<std::uint64_t>::max)();
    std::uint64_t clockMaxGapNs_ = 0;
    std::uint64_t clockMaxLateNs_ = 0;

    std::thread senderThread_;

    std::mutex observerMutex_;
    std::condition_variable observerReady_;
    std::deque<std::array<std::uint8_t, kDatagramBytes>> observerQueue_;
    bool observerStopping_ = false;
    std::uint64_t observerDrops_ = 0;
    std::thread observerThread_;
};

} // namespace dvbstreamer5::media::network
