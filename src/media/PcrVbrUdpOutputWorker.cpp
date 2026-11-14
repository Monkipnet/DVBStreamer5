#include "media/PcrVbrUdpOutputWorker.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>

namespace dvbstreamer5::media::network {
using Clock=std::chrono::steady_clock;
using namespace std::chrono_literals;
using dvbstreamer5::media::mpegts::Packet;
namespace {
constexpr std::size_t kMaxPackets=(8*1024*1024)/188;
constexpr std::size_t kMaximumObserverBytes=2U*1024U*1024U;
// Keep VBR startup bounded. The former 2.2s no-PCR hold delayed
// perfectly valid single-packet streams and failed the established
// native UDP relay regression. PCR-capable feeds still use the source
// PCR interval for all timed sends, without averaging the arrival rate.
// Source-PCR feeds need enough reservoir to smooth 0.5-1.0s HTTP bursts;
// the former averaged HTTP rate shaper is deliberately not in this path.
// Do not impose this delay on legacy MPEG-TS with no PCR clock.
constexpr auto kPcrPrebuffer=1200ms;
constexpr auto kMaxNoPcrWait=550ms;
constexpr auto kMaximumLate=80ms;
// Keep source PCR/PTS/DTS safety above burst suppression. If the peak cap
// would add >80ms beyond the source-PCR deadline, send uncapped rather than
// accumulating a decoder-underrun-inducing backlog.
constexpr auto kMaximumPeakCapDebt=80ms;
constexpr std::uint64_t kFallbackBps=4000000ULL;
}
PcrVbrUdpOutputWorker::PcrVbrUdpOutputWorker(
    UdpSocket* socket,std::size_t index,std::atomic<std::uint64_t>* global,
    std::atomic<std::uint64_t>* output,ErrorCallback onError,
    const std::string& streamId,ObserverCallback observer,
    std::uint64_t peakBitsPerSecond)
    :socket_(socket),index_(index),globalBytes_(global),outputBytes_(output),
     onError_(std::move(onError)),observer_(std::move(observer)),streamId_(streamId),
     peakBitsPerSecond_(peakBitsPerSecond){
    if(observer_)
        observerWorker_=std::thread(&PcrVbrUdpOutputWorker::dispatchObserver,this);
    try {
        worker_=std::thread(&PcrVbrUdpOutputWorker::run,this);
    }catch(...){
        {std::lock_guard<std::mutex> lock(observerMutex_);observerStop_=true;}
        observerCondition_.notify_all();
        if(observerWorker_.joinable())observerWorker_.join();
        throw;
    }
}
PcrVbrUdpOutputWorker::~PcrVbrUdpOutputWorker(){stop();}
void PcrVbrUdpOutputWorker::stop()noexcept{
    {std::lock_guard<std::mutex> lock(mutex_);stop_=true;}
    cv_.notify_all();
    if(worker_.joinable())worker_.join();
    {std::lock_guard<std::mutex> lock(observerMutex_);observerStop_=true;}
    observerCondition_.notify_all();
    if(observerWorker_.joinable())observerWorker_.join();
}
void PcrVbrUdpOutputWorker::dispatchObserver()noexcept {
    for(;;){
        std::vector<std::uint8_t> bytes;
        {
            std::unique_lock<std::mutex> lock(observerMutex_);
            observerCondition_.wait(lock,[&]{
                return observerStop_||!observerQueue_.empty();
            });
            if(observerStop_)break;
            bytes=std::move(observerQueue_.front());
            observerQueuedBytes_-=bytes.size();
            observerQueue_.pop_front();
        }
        try{
            observer_(bytes.data(),bytes.size());
        }catch(const std::exception& e){
            if(onError_)onError_("VBR fanout callback: "+std::string(e.what()));
            break;
        }catch(...){
            if(onError_)onError_("VBR fanout callback failed");
            break;
        }
    }
}
bool PcrVbrUdpOutputWorker::enqueue(const std::vector<Packet>& in){
    if(in.empty())return true;
    std::lock_guard<std::mutex> lock(mutex_);
    if(stop_)return false;
    if(packets_.empty())firstQueued_=Clock::now();
    for(const auto& packet:in){
        if(clock_.observe(packet,nextIndex_)){
            // Source has explicitly restarted its PCR clock. Old media cannot
            // be scheduled against the new timeline.
            droppedBytes_+=packets_.size()*188ULL;
            packets_.clear();
            frontIndex_=nextIndex_;
            firstQueued_=Clock::now();
            restartClock_=true;
            std::cerr<<"NATIVE UDP VBR PCR RESET stream_id="<<streamId_
                     <<" output_index="<<index_
                     <<" cause=pcr_discontinuity_or_invalid_jump"<<std::endl;
        }
        packets_.push_back(packet);
        ++nextIndex_;
    }
    if(packets_.size()>kMaxPackets){
        const auto excess=packets_.size()-kMaxPackets;
        for(std::size_t i=0;i<excess;++i)packets_.pop_front();
        frontIndex_+=excess;
        droppedBytes_+=excess*188ULL;
        firstQueued_=Clock::now();
        restartClock_=true;
        std::cerr<<"NATIVE UDP VBR PCR OVERFLOW stream_id="<<streamId_
                 <<" output_index="<<index_
                 <<" dropped_bytes="<<droppedBytes_
                 <<" reason=bounded_live_queue"<<std::endl;
    }
    cv_.notify_one();
    return true;
}
void PcrVbrUdpOutputWorker::run()noexcept{
    bool started=false,pcrMode=false;
    auto wallAnchor=Clock::time_point{};
    std::int64_t mediaAnchor=0;
    std::uint64_t indexAnchor=0;
    auto lastReport=Clock::now();
    std::uint64_t datagrams=0,lateResets=0;
    std::uint64_t peakThrottled=0,peakDeadlineBypass=0;
    auto lastWireSendAt=Clock::time_point{};
    std::size_t lastWireBytes=0;
    for(;;){
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock,[&]{return stop_||!packets_.empty();});
        if(stop_)break;
        const auto now=Clock::now();
        if(restartClock_){
            started=false;restartClock_=false;
            lastWireSendAt={};lastWireBytes=0;
        }
        if(!started){
            const auto elapsed=now-firstQueued_;
            const auto reservoir=clock_.ready()
                ? kPcrPrebuffer : kMaxNoPcrWait;
            if(elapsed<reservoir){
                cv_.wait_until(lock,firstQueued_+reservoir,
                    [&]{return stop_||restartClock_;});
                continue;
            }
            started=true;
            lastWireSendAt={};lastWireBytes=0;
            wallAnchor=now;
            indexAnchor=frontIndex_;
            pcrMode=clock_.timeNs(frontIndex_,mediaAnchor);
            if(!pcrMode)mediaAnchor=0;
            std::cerr<<"NATIVE UDP VBR PCR START stream_id="<<streamId_
                     <<" output_index="<<index_
                     <<" mode="<<(pcrMode?"source_pcr":"fallback_no_pcr")
                     <<" prebuffer_ms="<<
                        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
                     <<" pcr_pid="<<clock_.referencePid()
                     <<" vbr_peak_cap_kbps="<<peakBitsPerSecond_/1000ULL
                     <<" cap_max_debt_ms=80"
                     <<std::endl;
        }
        std::int64_t mediaNs=0;
        if(clock_.ready() && !pcrMode){
            pcrMode=clock_.timeNs(frontIndex_,mediaNs);
            mediaAnchor=mediaNs;indexAnchor=frontIndex_;wallAnchor=now;
        }
        if(pcrMode && !clock_.timeNs(frontIndex_,mediaNs)){
            pcrMode=false;indexAnchor=frontIndex_;wallAnchor=now;
        }
        if(!pcrMode){
            mediaNs=static_cast<std::int64_t>(
                static_cast<unsigned __int128>(frontIndex_-indexAnchor)*
                188ULL*8ULL*1000000000ULL/kFallbackBps);
            mediaAnchor=0;
        }
        const auto delta=mediaNs-mediaAnchor;
        auto due=wallAnchor+std::chrono::nanoseconds(delta);
        // Automatic V518 packet-burst shaping is per output. It never
        // modifies any TS byte, inserts NULLs, or stretches the source-PCR
        // timeline by more than the bounded 80-ms deadline allowance.
        // Time spacing is based on the *previous* datagram's size and
        // actual send time, not on bursty enqueue/producer timestamps.
        if(peakBitsPerSecond_>=4000000ULL && lastWireBytes &&
           lastWireSendAt.time_since_epoch().count()!=0){
            const auto intervalNs=static_cast<std::int64_t>(
                (static_cast<std::uint64_t>(lastWireBytes)*8000000000ULL +
                 peakBitsPerSecond_-1ULL)/peakBitsPerSecond_);
            const auto peakDue=lastWireSendAt+std::chrono::nanoseconds(intervalNs);
            if(peakDue>due){
                if(peakDue-due<=kMaximumPeakCapDebt){
                    due=peakDue;
                    ++peakThrottled;
                }else{
                    ++peakDeadlineBypass;
                }
            }
        }
        if(due>now){
            cv_.wait_until(lock,due,[&]{return stop_||restartClock_;});
            continue;
        }
        if(now-due>kMaximumLate){
            // Real scheduler stall/underflow: discard only old wall-clock
            // phase, not TS bytes or their PCR/PTS/DTS.
            wallAnchor=now-std::chrono::nanoseconds(delta);
            ++lateResets;
        }
        std::array<std::uint8_t,7*188> buffer{};
        const auto count=std::min<std::size_t>(7,packets_.size());
        for(std::size_t i=0;i<count;++i){
            std::memcpy(buffer.data()+i*188,packets_.front().data(),188);
            packets_.pop_front();++frontIndex_;
        }
        const auto queued=packets_.size();
        const auto dropped=droppedBytes_;
        lock.unlock();
        std::string error;
        // The network socket is always served before any observer work.
        if(socket_){
            if(!socket_->send(buffer.data(),count*188,error)){
                if(onError_)onError_("UDP-VBR PCR sender: "+error);
                return;
            }
            if(globalBytes_)globalBytes_->fetch_add(count*188ULL,std::memory_order_relaxed);
            if(outputBytes_)outputBytes_->fetch_add(count*188ULL,std::memory_order_relaxed);
        }
        if(observer_){
            const std::size_t bytes=count*188;
            std::lock_guard<std::mutex> guard(observerMutex_);
            if(!observerStop_){
                // Only observer copies may be discarded when a slow output
                // exceeds 2 MiB; physical UDP transport remains unaffected.
                while(observerQueuedBytes_+bytes>kMaximumObserverBytes &&
                      !observerQueue_.empty()){
                    observerDroppedBytes_+=observerQueue_.front().size();
                    observerQueuedBytes_-=observerQueue_.front().size();
                    observerQueue_.pop_front();
                }
                observerQueue_.emplace_back(buffer.data(),buffer.data()+bytes);
                observerQueuedBytes_+=bytes;
                observerCondition_.notify_one();
            }
        }
        if(peakBitsPerSecond_){
            lastWireSendAt=Clock::now();
            lastWireBytes=count*188;
        }
        ++datagrams;
        if(now-lastReport>15s){
            std::cerr<<"NATIVE UDP VBR PCR STATUS stream_id="<<streamId_
                     <<" output_index="<<index_
                     <<" mode="<<(pcrMode?"source_pcr":"fallback_no_pcr")
                     <<" queue_packets="<<queued
                     <<" late_reanchors="<<lateResets
                     <<" vbr_peak_cap_kbps="<<peakBitsPerSecond_/1000ULL
                     <<" cap_throttled_datagrams="<<peakThrottled
                     <<" cap_deadline_bypass="<<peakDeadlineBypass
                     <<" dropped_bytes="<<dropped
                     <<" observer_dropped_bytes="<<observerDroppedBytes_
                     <<" datagrams="<<datagrams<<std::endl;
            lastReport=now;
        }
    }
}
} // namespace dvbstreamer5::media::network
