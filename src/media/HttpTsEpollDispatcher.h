#pragma once
#if !defined(__linux__)
#error "HTTP TS epoll dispatcher requires Linux"
#endif

#include <sys/epoll.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <atomic>
#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <iostream>

namespace dvbstreamer5::media::network {

// A fixed number of pollers serves any number of HTTP-MPEGTS connections.
// All sockets are nonblocking. An upstream backlog never blocks another
// client or a shared TS publisher; the existing hub evicts slow subscribers.
class HttpTsEpollDispatcher {
public:
    using OnClose = std::function<void(const char*,std::uint64_t)>;
    explicit HttpTsEpollDispatcher(std::size_t workers=4) {
        if(!workers)workers=1;
        for(std::size_t i=0;i<workers;++i)
            workers_.push_back(std::make_unique<Worker>());
    }
    ~HttpTsEpollDispatcher(){stop();}
    HttpTsEpollDispatcher(const HttpTsEpollDispatcher&)=delete;
    HttpTsEpollDispatcher& operator=(const HttpTsEpollDispatcher&)=delete;

    bool add(int fd,int hubFd,OnClose onClose) {
        if(fd<0||hubFd<0)return false;
        const auto index=next_.fetch_add(1,std::memory_order_relaxed)%workers_.size();
        return workers_[index]->add(fd,hubFd,std::move(onClose));
    }
    void stop()noexcept {
        for(auto& worker:workers_)worker->stop();
    }
private:
    struct Client {
        int fd=-1,hubFd=-1;
        bool readEnabled=true,wantWrite=false;
        std::deque<std::vector<char>> pending;
        std::size_t queued=0,offset=0;
        std::uint64_t sent=0;
        OnClose onClose;
    };
    class Worker {
    public:
        Worker(){
            epfd_=::epoll_create1(EPOLL_CLOEXEC);
            if(epfd_<0)throw std::runtime_error("HTTP epoll_create1 failed");
            try{thread_=std::thread([this]{run();});}
            catch(...){::close(epfd_);throw;}
        }
        ~Worker(){stop();}
        bool add(int fd,int hubFd,OnClose closeCallback){
            const auto makeNonblocking=[](int value){
                const int flags=::fcntl(value,F_GETFL,0);
                return flags>=0 && ::fcntl(value,F_SETFL,flags|O_NONBLOCK)==0;
            };
            if(!makeNonblocking(fd)||!makeNonblocking(hubFd))return false;
            auto client=std::make_shared<Client>();
            client->fd=fd;client->hubFd=hubFd;
            client->onClose=std::move(closeCallback);
            std::lock_guard<std::mutex> guard(mutex_);
            if(stopped_.load(std::memory_order_relaxed))return false;
            struct epoll_event upstream{};
            upstream.events=EPOLLIN|EPOLLRDHUP|EPOLLERR|EPOLLHUP;
            upstream.data.fd=hubFd;
            if(::epoll_ctl(epfd_,EPOLL_CTL_ADD,hubFd,&upstream)<0)return false;
            struct epoll_event downstream{};
            downstream.events=EPOLLRDHUP|EPOLLERR|EPOLLHUP;
            downstream.data.fd=fd;
            if(::epoll_ctl(epfd_,EPOLL_CTL_ADD,fd,&downstream)<0){
                ::epoll_ctl(epfd_,EPOLL_CTL_DEL,hubFd,nullptr);
                return false;
            }
            sockets_[fd]=client;
            sockets_[hubFd]=client;
            return true;
        }
        void stop()noexcept {
            stopped_.store(true,std::memory_order_release);
            if(thread_.joinable())thread_.join();
            std::vector<std::shared_ptr<Client>> unclosed;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                for(auto& [fd,c]:sockets_)
                    if(fd==c->fd)unclosed.push_back(c);
                sockets_.clear();
            }
            for(auto& client:unclosed){
                ::epoll_ctl(epfd_,EPOLL_CTL_DEL,client->hubFd,nullptr);
                ::epoll_ctl(epfd_,EPOLL_CTL_DEL,client->fd,nullptr);
                client->onClose("dispatcher_stop",client->sent);
            }
            if(epfd_>=0){::close(epfd_);epfd_=-1;}
        }
    private:
        static constexpr std::size_t kPauseRead=512*1024;
        static constexpr std::size_t kResumeRead=128*1024;
        int epfd_=-1;
        std::atomic<bool> stopped_{false};
        std::thread thread_;
        std::mutex mutex_;
        std::unordered_map<int,std::shared_ptr<Client>> sockets_;

        void setRead(Client& c,bool enabled){
            if(c.readEnabled==enabled)return;
            struct epoll_event ev{};
            ev.data.fd=c.hubFd;
            ev.events=EPOLLRDHUP|EPOLLERR|EPOLLHUP|(enabled?EPOLLIN:0U);
            if(::epoll_ctl(epfd_,EPOLL_CTL_MOD,c.hubFd,&ev)==0)
                c.readEnabled=enabled;
        }
        void setWrite(Client& c,bool enabled){
            if(c.wantWrite==enabled)return;
            struct epoll_event ev{};
            ev.data.fd=c.fd;
            ev.events=EPOLLRDHUP|EPOLLERR|EPOLLHUP|(enabled?EPOLLOUT:0U);
            if(::epoll_ctl(epfd_,EPOLL_CTL_MOD,c.fd,&ev)==0)
                c.wantWrite=enabled;
        }
        bool flush(Client& c){
            std::size_t budget=128;
            while(!c.pending.empty()&&budget--){
                const auto& chunk=c.pending.front();
                const auto bytes=chunk.size()-c.offset;
                const auto n=::send(c.fd,chunk.data()+c.offset,bytes,
                                    MSG_DONTWAIT|MSG_NOSIGNAL);
                if(n>0){
                    c.sent+=static_cast<std::uint64_t>(n);
                    c.queued-=static_cast<std::size_t>(n);
                    c.offset+=static_cast<std::size_t>(n);
                    if(c.offset==chunk.size()){
                        c.pending.pop_front();
                        c.offset=0;
                    }
                }else if(n<0&&errno==EINTR)continue;
                else if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK))break;
                else return false;
            }
            setWrite(c,!c.pending.empty());
            if(c.queued<=kResumeRead)setRead(c,true);
            return true;
        }
        bool ingest(Client& c){
            std::array<char,64*1024> packet{};
            for(unsigned count=0;count<64 && c.readEnabled;++count){
                const auto n=::recv(c.hubFd,packet.data(),packet.size(),MSG_DONTWAIT);
                if(n>0){
                    c.pending.emplace_back(packet.data(),packet.data()+n);
                    c.queued+=static_cast<std::size_t>(n);
                    if(c.queued>=kPauseRead)setRead(c,false);
                }else if(n<0&&errno==EINTR)continue;
                else if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK))break;
                else return false;
            }
            return flush(c);
        }
        void remove(const std::shared_ptr<Client>& client,const char* reason){
            {
                std::lock_guard<std::mutex> guard(mutex_);
                auto it=sockets_.find(client->fd);
                if(it==sockets_.end()||it->second.get()!=client.get())return;
                sockets_.erase(client->fd);
                sockets_.erase(client->hubFd);
                ::epoll_ctl(epfd_,EPOLL_CTL_DEL,client->fd,nullptr);
                ::epoll_ctl(epfd_,EPOLL_CTL_DEL,client->hubFd,nullptr);
            }
            // Never execute StreamManager/NativePreviewHub callback under a
            // reactor lock. Subscriber disconnect affects only that viewer.
            client->onClose(reason,client->sent);
        }
        void run()noexcept {
            std::array<epoll_event,128> events{};
            while(!stopped_.load(std::memory_order_acquire)){
                const int ready=::epoll_wait(epfd_,events.data(),
                                  static_cast<int>(events.size()),100);
                if(ready<0){if(errno==EINTR)continue;break;}
                for(int i=0;i<ready;++i){
                    std::shared_ptr<Client> client;
                    const int fd=events[static_cast<std::size_t>(i)].data.fd;
                    {
                        std::lock_guard<std::mutex> guard(mutex_);
                        const auto it=sockets_.find(fd);
                        if(it!=sockets_.end())client=it->second;
                    }
                    if(!client)continue;
                    const auto flags=events[static_cast<std::size_t>(i)].events;
                    if(flags&(EPOLLERR|EPOLLHUP|EPOLLRDHUP)){
                        remove(client,"socket_closed");continue;
                    }
                    if(fd==client->hubFd&&(flags&EPOLLIN)){
                        if(!ingest(*client))remove(client,"hub_eof");
                    }else if(fd==client->fd&&(flags&EPOLLOUT)){
                        if(!flush(*client))remove(client,"tcp_send_error");
                    }
                }
            }
        }
    };
    std::vector<std::unique_ptr<Worker>> workers_;
    std::atomic<std::size_t> next_{0};
};
} // namespace dvbstreamer5::media::network
