#include "media/UdpTsOutput.h"
#include "media/UdpSendClock.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#ifdef __linux__
#include <sys/prctl.h>
#endif

namespace dvbstreamer5::media::network {
namespace {
std::uint64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
constexpr std::uint64_t kRetryNs=250000ULL;
constexpr std::size_t kPendingDatagrams=512, kObserverDatagrams=4096;
}

struct UdpTsOutput::State {
    struct Frame { UdpTsMux::Datagram data; std::uint64_t created; };
    struct Endpoint { Destination target; std::deque<std::shared_ptr<Frame>> pending; std::uint64_t retryAt=0; };
    State(bool constant, std::uint64_t rate) : cbr(constant), mux(constant,rate),
        clock(std::max<std::uint64_t>(1,mux.bitrate())) {}
    mutable std::mutex mutex;
    std::condition_variable observerReady;
    const bool cbr;
    UdpTsMux mux;
    UdpSendClock clock;
    std::vector<Endpoint> endpoints;
    std::atomic<std::uint64_t>* total=nullptr;
    Observer observer;
    Failure failure;
    Sender sender;
    std::thread observerThread;
    std::deque<std::shared_ptr<Frame>> observed;
    bool stopped=false, finished=false, clockStarted=false, observerBusy=false;
    std::uint64_t firstInput=0, next=0;
    Stats counters;
    std::string error;

    void fail(std::string message) {
        if (error.empty()) error=std::move(message);
        stopped=true; observerReady.notify_all();
    }
    bool sendPending(std::uint64_t now) {
        for (auto& ep:endpoints) {
            // One attempt per destination per turn: a blocked destination
            // cannot hold the scheduler or cause a burst on another socket.
            if (ep.pending.empty() || now<ep.retryAt) continue;
            const auto& frame=*ep.pending.front();
            if (now-frame.created>500000000ULL) {
                fail("UDP output index="+std::to_string(ep.target.index)+" has a datagram pending for over 500 ms");
                return false;
            }
            std::string detail;
            const auto result=sender
                ? sender(*ep.target.socket,ep.target.index,frame.data.bytes.data(),frame.data.size,detail)
                : ep.target.socket->trySend(frame.data.bytes.data(),frame.data.size,detail);
            if (result==UdpSocket::SendResult::Retry) {
                ++counters.retries; ep.retryAt=now+kRetryNs; continue;
            }
            if (result==UdpSocket::SendResult::Failed) {
                fail("UDP output index="+std::to_string(ep.target.index)+": "+detail); return false;
            }
            total->fetch_add(frame.data.size,std::memory_order_relaxed);
            if (ep.target.sentBytes) ep.target.sentBytes->fetch_add(frame.data.size,std::memory_order_relaxed);
            ++counters.datagrams; ep.pending.pop_front();
            ep.retryAt=now+(cbr ? (1316ULL*8ULL*1000000000ULL/mux.bitrate())*9ULL/10ULL : 1000ULL);
        }
        return true;
    }
    std::uint64_t tick(std::uint64_t now) {
        if (stopped) return now+20000000ULL;
        if (!sendPending(now)) return now+20000000ULL;
        if (!mux.started()) return now+20000000ULL;
        if (!clockStarted) {
            next=firstInput+mux.startupNs();
            clock.start(next); clockStarted=true;
        }
        if (now>=next && (!finished || mux.queuedPackets())) {
            UdpTsMux::Datagram data;
            if (mux.emit(now,data,finished)) {
                if (cbr && !data.useful) ++counters.nullDatagrams;
                auto frame=std::make_shared<Frame>(Frame{std::move(data),now});
                for (auto& ep:endpoints) {
                    if (ep.pending.size()>=kPendingDatagrams) {
                        fail("UDP destination queue full, index="+std::to_string(ep.target.index));
                        return now+20000000ULL;
                    }
                    ep.pending.push_back(frame);
                }
                if (observer) {
                    if (observed.size()>=kObserverDatagrams) {
                        fail("UDP observer queue full; transport observer is too slow");
                        return now+20000000ULL;
                    }
                    observed.push_back(frame); observerReady.notify_one();
                }
                if (cbr) {
                    const auto sent=nowNs();
                    const auto due=clock.deadline();
                    if (sent>due) counters.lateMaxUs=std::max<std::uint64_t>(counters.lateMaxUs,(sent-due)/1000ULL);
                    if (clock.advance(sent)) ++counters.clockResets;
                    next=clock.deadline();
                } else next=now; // VBR follows source availability; no stuffing
                if (!sendPending(nowNs())) return now+20000000ULL;
            } else next=now+1000000ULL; // bounded partial-VBR latency
        }
        auto wake=finished && !mux.queuedPackets() ? now+20000000ULL : next;
        if (!cbr && !mux.queuedPackets()) wake=now+20000000ULL;
        for (const auto& ep:endpoints) if (!ep.pending.empty()) wake=std::min<std::uint64_t>(wake,ep.retryAt);
        return std::max<std::uint64_t>(wake,now+1000ULL);
    }
    void observeLoop() {
        for (;;) {
            std::shared_ptr<Frame> frame;
            std::string failureText;
            {
                std::unique_lock<std::mutex> lock(mutex);
                observerReady.wait(lock,[&]{return stopped || !observed.empty();});
                if (!error.empty()) { failureText=error; observed.clear(); }
                else if (!observed.empty()) { frame=observed.front(); observed.pop_front(); observerBusy=true; }
                else if (stopped) break;
            }
            if (!failureText.empty()) {
                try { if (failure) failure(failureText); } catch (...) {}
                break;
            }
            try { if (frame && observer) observer(frame->data.bytes.data(),frame->data.size); }
            catch (const std::exception& e) { std::lock_guard<std::mutex> lock(mutex); fail(std::string("UDP observer: ")+e.what()); }
            catch (...) { std::lock_guard<std::mutex> lock(mutex); fail("UDP observer failed"); }
            { std::lock_guard<std::mutex> lock(mutex); observerBusy=false; }
        }
    }
};

// Two shared scheduler threads, not a timer/spinning RT thread per destination.
// Snapshot shared_ptrs protect state lifetime; no callback runs under these locks.
class UdpOutputScheduler {
public:
    static UdpOutputScheduler& instance() { static UdpOutputScheduler scheduler; return scheduler; }
    unsigned add(const std::shared_ptr<UdpTsOutput::State>& state) {
        const auto index=(state->cbr ? cbrRoundRobin_ : vbrRoundRobin_).fetch_add(1)%2U;
        auto& shard=shards_[index];
        { std::lock_guard<std::mutex> lock(shard.mutex); shard.states.push_back(state); ++shard.generation; }
        shard.ready.notify_one(); return index;
    }
    void wake(unsigned index) {
        auto& shard=shards_[index];
        { std::lock_guard<std::mutex> lock(shard.mutex); ++shard.generation; }
        shard.ready.notify_one();
    }
    void remove(unsigned index,const std::shared_ptr<UdpTsOutput::State>& state) {
        auto& shard=shards_[index];
        { std::lock_guard<std::mutex> lock(shard.mutex);
          shard.states.erase(std::remove(shard.states.begin(),shard.states.end(),state),shard.states.end()); ++shard.generation; }
        shard.ready.notify_one();
    }
private:
    struct Shard {
        std::mutex mutex;
        std::condition_variable ready;
        std::vector<std::shared_ptr<UdpTsOutput::State>> states;
        std::uint64_t generation=0;
        bool stop=false;
        std::thread worker;
    } shards_[2];
    std::atomic<unsigned> cbrRoundRobin_{0},vbrRoundRobin_{0};
    UdpOutputScheduler() {
        try {
            for (auto& shard:shards_) {
                auto* target=&shard;
                shard.worker=std::thread([this,target]{run(*target);});
            }
        } catch (...) {
            for (auto& shard:shards_) {
                { std::lock_guard<std::mutex> lock(shard.mutex); shard.stop=true; }
                shard.ready.notify_all();
            }
            for (auto& shard:shards_) if (shard.worker.joinable()) shard.worker.join();
            throw;
        }
    }
    ~UdpOutputScheduler() {
        for (auto& shard:shards_) { {std::lock_guard<std::mutex> lock(shard.mutex);shard.stop=true;} shard.ready.notify_all(); }
        for (auto& shard:shards_) shard.worker.join();
    }
    void run(Shard& shard) {
#ifdef __linux__
        (void)::prctl(PR_SET_TIMERSLACK,1UL,0UL,0UL,0UL);
#endif
        auto wakeAt=nowNs();
        std::unique_lock<std::mutex> lock(shard.mutex);
        while (!shard.stop) {
            const auto generation=shard.generation;
            const auto states=shard.states;
            lock.unlock();
            wakeAt=nowNs()+20000000ULL;
            for (const auto& state:states) {
                // A producer copying an input block must not hold up every
                // other channel assigned to this scheduler shard.
                std::unique_lock<std::mutex> stateLock(state->mutex,std::try_to_lock);
                if (!stateLock.owns_lock()) {
                    wakeAt=std::min<std::uint64_t>(wakeAt,nowNs()+100000ULL);
                    continue;
                }
                try { wakeAt=std::min(wakeAt,state->tick(nowNs())); }
                catch (const std::exception& e) { state->fail(std::string("UDP scheduler: ")+e.what()); }
                catch (...) { state->fail("UDP scheduler failed"); }
            }
            lock.lock();
            shard.ready.wait_until(lock,std::chrono::steady_clock::time_point(std::chrono::nanoseconds(wakeAt)),
                [&]{return shard.stop || generation!=shard.generation;});
        }
    }
};

UdpTsOutput::UdpTsOutput(bool cbr,std::uint64_t bitrate,std::string id,
    std::vector<Destination> destinations,std::atomic<std::uint64_t>* total,Observer observer,Failure failure,Sender sender)
    : state_(std::make_shared<State>(cbr,bitrate)) {
    if (!total || destinations.empty()) throw std::invalid_argument("UDP output requires destinations/counters");
    state_->total=total; state_->observer=std::move(observer); state_->failure=std::move(failure);
    state_->counters.cbr=cbr;
    state_->sender=std::move(sender);
    for (auto& destination:destinations) {
        int buffer=0; std::string error;
        if (!destination.socket || !destination.socket->configureOutputBuffer(4*1024*1024,buffer,error))
            throw std::runtime_error("UDP output socket setup: "+error);
        state_->counters.sendBufferBytes=state_->counters.sendBufferBytes ?
            std::min<std::uint64_t>(state_->counters.sendBufferBytes,buffer) : buffer;
        state_->endpoints.push_back({std::move(destination),{},0});
    }
    auto state=state_;
    state_->observerThread=std::thread([state]{state->observeLoop();});
    try { shard_=UdpOutputScheduler::instance().add(state_); }
    catch (...) { {std::lock_guard<std::mutex> lock(state_->mutex);state_->stopped=true;}state_->observerReady.notify_all();state_->observerThread.join();throw; }
    std::cerr<<"UDP TS OUTPUT engine=native stream_id="<<id<<" mode="<<(cbr?"cbr":"vbr")
        <<" destinations="<<state_->endpoints.size()<<" target_kbps="<<state_->mux.bitrate()/1000ULL
        <<" source_pcr="<<state_->mux.sourcePcr()<<" startup_ms="<<state_->mux.startupNs()/1000000ULL
        <<" sndbuf_bytes="<<state_->counters.sendBufferBytes<<" scheduler_shards=2"<<std::endl;
}
UdpTsOutput::~UdpTsOutput() { stop(); }
bool UdpTsOutput::enqueue(const std::vector<mpegts::Packet>& packets) {
    bool accepted,wake=false;
    { std::lock_guard<std::mutex> lock(state_->mutex);
      if (state_->stopped || state_->finished) return false;
      const auto time=nowNs(); const bool started=state_->mux.started();
      const auto before=state_->mux.queuedPackets();
      try { accepted=state_->mux.push(packets,time); }
      catch (const std::exception& e) { state_->fail(std::string("UDP queue: ")+e.what()); return false; }
      if (!accepted) {
          if (state_->mux.lastPushError()==UdpTsMux::PushError::QueueFull)
              state_->fail("UDP TS queue full: limit_bytes="+std::to_string(UdpTsMux::kMaxBytes)+
                  " target_kbps="+std::to_string(state_->mux.bitrate()/1000ULL)+"; inspect bitrate budget and sender delays");
          else state_->fail("invalid MPEG-TS packet at UDP output boundary");
      }
      if (!started && state_->mux.started()) state_->firstInput=time;
      wake=(!started && state_->mux.started()) || (!state_->cbr &&
          (!before || (before<7 && state_->mux.queuedPackets()>=7)));
      state_->counters.queuePeakBytes=std::max<std::uint64_t>(state_->counters.queuePeakBytes,state_->mux.queuedPackets()*188ULL);
    }
    if (wake) UdpOutputScheduler::instance().wake(shard_);
    return accepted;
}
void UdpTsOutput::finish() {
    { std::lock_guard<std::mutex> lock(state_->mutex); if (state_->finished) return; state_->finished=true; }
    UdpOutputScheduler::instance().wake(shard_);
}
void UdpTsOutput::stop() {
    if (!state_) return;
    { std::lock_guard<std::mutex> lock(state_->mutex);state_->stopped=true; }
    UdpOutputScheduler::instance().remove(shard_,state_); state_->observerReady.notify_all();
    if (state_->observerThread.joinable()) {
        if (state_->observerThread.get_id()==std::this_thread::get_id()) state_->observerThread.detach();
        else state_->observerThread.join();
    }
}
std::size_t UdpTsOutput::queuedPackets() const { std::lock_guard<std::mutex> lock(state_->mutex); return state_->mux.queuedPackets(); }
bool UdpTsOutput::drained() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return !state_->mux.queuedPackets() && state_->observed.empty() && !state_->observerBusy &&
        std::all_of(state_->endpoints.begin(),state_->endpoints.end(),[](const auto& ep){return ep.pending.empty();});
}
UdpTsOutput::Stats UdpTsOutput::stats() const {
    std::lock_guard<std::mutex> lock(state_->mutex); auto stats=state_->counters;
    stats.queueBytes=state_->mux.queuedPackets()*188ULL;
    for (const auto& ep:state_->endpoints) for (const auto& frame:ep.pending) stats.pendingBytes+=frame->data.size;
    for (const auto& frame:state_->observed) stats.observerBytes+=frame->data.size;
    return stats;
}
std::string UdpTsOutput::lastError() const { std::lock_guard<std::mutex> lock(state_->mutex);return state_->error; }

} // namespace dvbstreamer5::media::network
