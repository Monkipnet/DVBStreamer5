#pragma once
#include "media/PcrVbrSourceClock.h"
#include "media/UdpSocket.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dvbstreamer5::media::network {

// One PCR-disciplined UDP-VBR sender per output; never runs on the input
// reader/transcoder thread. A slow output cannot block other destinations.
class PcrVbrUdpOutputWorker {
public:
    using ErrorCallback=std::function<void(const std::string&)>;
    // Called once per PCR-paced batch before network fan-out. HLS segments,
    // HTTP TS, SRT, RTSP and RTMP all consume the same media timeline.
    using ObserverCallback=std::function<void(const std::uint8_t*,std::size_t)>;
    PcrVbrUdpOutputWorker(UdpSocket* socket,std::size_t index,
                          std::atomic<std::uint64_t>* globalBytes,
                          std::atomic<std::uint64_t>* outputBytes,
                          ErrorCallback onError,const std::string& streamId,
                          ObserverCallback observer={},
                          std::uint64_t peakBitsPerSecond=0);
    ~PcrVbrUdpOutputWorker();
    PcrVbrUdpOutputWorker(const PcrVbrUdpOutputWorker&)=delete;
    PcrVbrUdpOutputWorker& operator=(const PcrVbrUdpOutputWorker&)=delete;
    bool enqueue(const std::vector<dvbstreamer5::media::mpegts::Packet>& packets);
    void stop()noexcept;
private:
    void run()noexcept;
    void dispatchObserver()noexcept;
    UdpSocket* socket_=nullptr;
    std::size_t index_=0;
    std::atomic<std::uint64_t>* globalBytes_=nullptr;
    std::atomic<std::uint64_t>* outputBytes_=nullptr;
    ErrorCallback onError_;
    ObserverCallback observer_;
    std::string streamId_;
    std::uint64_t peakBitsPerSecond_=0;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<dvbstreamer5::media::mpegts::Packet> packets_;
    dvbstreamer5::media::mpegts::PcrVbrSourceClock clock_;
    std::uint64_t frontIndex_=0,nextIndex_=0,droppedBytes_=0;
    std::chrono::steady_clock::time_point firstQueued_{};
    bool stop_=false;
    bool restartClock_=false;
    // Fanout operations may block (e.g. HLS segment writes). A bounded queue
    // prevents them from delaying the source-PCR UDP sender clock.
    std::mutex observerMutex_;
    std::condition_variable observerCondition_;
    std::deque<std::vector<std::uint8_t>> observerQueue_;
    std::size_t observerQueuedBytes_=0;
    std::uint64_t observerDroppedBytes_=0;
    bool observerStop_=false;
    std::thread observerWorker_;
    std::thread worker_;
};

} // namespace dvbstreamer5::media::network
