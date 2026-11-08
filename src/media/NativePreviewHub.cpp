#include "NativePreviewHub.h"

#include "TransportStream.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <iostream>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace dvbstreamer5::media::network {

NativePreviewHub::~NativePreviewHub() {
    close();
}

int NativePreviewHub::subscribe(std::string& error) {
#if defined(_WIN32)
    error = "native preview socket fan-out is unavailable on Windows";
    return -1;
#else
    int sockets[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) != 0) {
        error = std::string("cannot create native preview socket pair: ") +
            std::strerror(errno);
        return -1;
    }
    const int flags = ::fcntl(sockets[1], F_GETFL, 0);
    if (flags < 0 || ::fcntl(sockets[1], F_SETFL, flags | O_NONBLOCK) != 0) {
        error = std::string("cannot make native preview nonblocking: ") +
            std::strerror(errno);
        ::close(sockets[0]);
        ::close(sockets[1]);
        return -1;
    }
    // Reserve enough kernel space for bursty producer scheduling. Preserve
    // packet ordering and the bounded user-space queue on sustained stalls.
    const int queueBytes = 2 * 1024 * 1024;
    ::setsockopt(sockets[1], SOL_SOCKET, SO_SNDBUF, &queueBytes, sizeof(queueBytes));
    ::setsockopt(sockets[0], SOL_SOCKET, SO_RCVBUF, &queueBytes, sizeof(queueBytes));

    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) {
        error = "native preview hub is closed";
        ::close(sockets[0]);
        ::close(sockets[1]);
        return -1;
    }
    auto sub=std::make_shared<Subscriber>();
    sub->sendFd=sockets[1];
    subscribers_.emplace(sockets[0],std::move(sub));
    return sockets[0];
#endif
}

void NativePreviewHub::unsubscribe(int readFd) {
#if !defined(_WIN32)
    std::shared_ptr<Subscriber> subscriber;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it=subscribers_.find(readFd);
        if(it==subscribers_.end())return;
        subscriber=std::move(it->second);
        subscribers_.erase(it);
    }
    // A producer may be publishing using a snapshot of the same subscriber.
    // Its own lock prevents sending on a closed/reused descriptor.
    std::lock_guard<std::mutex> clientLock(subscriber->mutex);
    subscriber->detached=true;
    if(subscriber->sendFd>=0){
        ::close(subscriber->sendFd);
        subscriber->sendFd=-1;
    }
#else
    (void)readFd;
#endif
}

void NativePreviewHub::publish(const std::uint8_t* data, std::size_t size) {
#if !defined(_WIN32)
    if(!data||size<dvbstreamer5::media::mpegts::kPacketSize)return;
    constexpr std::size_t kChunk=7*dvbstreamer5::media::mpegts::kPacketSize;
    const auto aligned=size-(size%dvbstreamer5::media::mpegts::kPacketSize);
    std::vector<std::pair<int,std::shared_ptr<Subscriber>>> subscribers;
    bool publishReport=false;
    std::uint64_t bytes=0,calls=0,rateMs=0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(closed_)return;
        subscribers.reserve(subscribers_.size());
        for(const auto& item:subscribers_)
            subscribers.emplace_back(item.first,item.second);
        const auto now=std::chrono::steady_clock::now();
        if(lastRateSample_==std::chrono::steady_clock::time_point{})
            lastRateSample_=now;
        publishedBytes_+=aligned;
        ++publishedCalls_;
        rateMs=static_cast<std::uint64_t>(std::chrono::duration_cast<
            std::chrono::milliseconds>(now-lastRateSample_).count());
        if(rateMs>=5000){
            publishReport=!subscribers.empty();
            bytes=publishedBytes_;calls=publishedCalls_;
            lastRateSample_=now;publishedBytes_=0;publishedCalls_=0;
        }
    }
    // Each pending chunk is materialized at most once for all subscribers.
    // When every subscriber keeps up, no heap copy is made at all.
    std::vector<std::shared_ptr<const std::vector<std::uint8_t>>> shared(
        (aligned+kChunk-1)/kChunk);
    std::vector<std::pair<int,std::shared_ptr<Subscriber>>> dead;
    std::size_t largestPending=0;
    for(const auto& [fd,subscriber]:subscribers){
        bool remove=false;
        const char* reason="unknown";
        int error=0;
        {
            std::lock_guard<std::mutex> clientLock(subscriber->mutex);
            if(subscriber->detached||subscriber->sendFd<0)continue;
            auto sendPacket=[&](const std::uint8_t* ptr,std::size_t bytes){
                const auto count=::send(subscriber->sendFd,ptr,bytes,
                                         MSG_DONTWAIT|MSG_NOSIGNAL);
                if(count==static_cast<ssize_t>(bytes))return 0;
                if(count<0&&(errno==EAGAIN||errno==EWOULDBLOCK||errno==ENOBUFS))
                    return 1;
                error=count<0?errno:0;
                reason=count<0?"socket_send_error":"short_seqpacket_send";
                return -1;
            };
            for(;!subscriber->pending.empty();){
                const auto& packet=subscriber->pending.front();
                const int result=sendPacket(packet->data(),packet->size());
                if(result==1)break;
                if(result<0){remove=true;break;}
                subscriber->pendingBytes-=packet->size();
                subscriber->pending.pop_front();
            }
            if(subscriber->pendingBytes<1024U*1024U)
                subscriber->backlogWarningLogged=false;
            if(subscriber->pendingBytes>=2U*1024U*1024U &&
               !subscriber->backlogWarningLogged){
                std::cerr<<"NATIVE HTTP TS HUB backlog"
                         <<" hub_fd="<<fd
                         <<" pending_bytes="<<subscriber->pendingBytes<<std::endl;
                subscriber->backlogWarningLogged=true;
            }
            for(std::size_t off=0;!remove&&off<aligned;){
                const auto chunk=std::min(kChunk,aligned-off);
                if(subscriber->pending.empty()){
                    const int result=sendPacket(data+off,chunk);
                    if(result==0){off+=chunk;continue;}
                    if(result<0){remove=true;break;}
                }
                if(subscriber->pendingBytes+chunk>kMaxPendingBytes){
                    reason="pending_limit";
                    remove=true;break;
                }
                auto& payload=shared[off/kChunk];
                if(!payload)payload=std::make_shared<
                    const std::vector<std::uint8_t>>(data+off,data+off+chunk);
                subscriber->pending.push_back(payload);
                subscriber->pendingBytes+=chunk;
                off+=chunk;
            }
            largestPending=std::max(largestPending,subscriber->pendingBytes);
            if(remove){
                std::cerr<<"NATIVE HTTP TS HUB detach"
                         <<" hub_fd="<<fd<<" reason="<<reason
                         <<" errno="<<error
                         <<" pending_bytes="<<subscriber->pendingBytes
                         <<std::endl;
                subscriber->detached=true;
                ::close(subscriber->sendFd);
                subscriber->sendFd=-1;
            }
        }
        if(remove)dead.emplace_back(fd,subscriber);
    }
    if(!dead.empty()){
        std::lock_guard<std::mutex> lock(mutex_);
        for(const auto& [fd,pointer]:dead){
            const auto found=subscribers_.find(fd);
            if(found!=subscribers_.end()&&found->second==pointer)
                subscribers_.erase(found);
        }
    }
    if(publishReport){
        std::cerr<<"NATIVE HTTP TS HUB PRODUCER"
                 <<" output_kbps="<<(rateMs?bytes*8ULL/rateMs:0)
                 <<" publish_calls="<<calls
                 <<" subscriber_count="<<subscribers.size()
                 <<" max_pending_bytes="<<largestPending
                 <<" fanout_lock=per_subscriber"
                 <<std::endl;
    }
#else
    (void)data;(void)size;
#endif
}

void NativePreviewHub::flushPending() {
#if !defined(_WIN32)
    std::vector<std::shared_ptr<Subscriber>> current;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(closed_)return;
        for(const auto& entry:subscribers_)current.push_back(entry.second);
    }
    for(const auto& sub:current){
        std::lock_guard<std::mutex> clientLock(sub->mutex);
        sub->pending.clear();
        sub->pendingBytes=0;
        sub->backlogWarningLogged=false;
    }
#endif
}

std::size_t NativePreviewHub::subscriberCount() {
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_?0U:subscribers_.size();
}

void NativePreviewHub::close() {
#if !defined(_WIN32)
    std::map<int,std::shared_ptr<Subscriber>> current;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(closed_)return;
        closed_=true;
        current.swap(subscribers_);
    }
    for(const auto& entry:current){
        const auto& subscriber=entry.second;
        std::lock_guard<std::mutex> clientLock(subscriber->mutex);
        subscriber->detached=true;
        if(subscriber->sendFd>=0){
            ::close(subscriber->sendFd);
            subscriber->sendFd=-1;
        }
    }
#endif
}

} // namespace dvbstreamer5::media::network
