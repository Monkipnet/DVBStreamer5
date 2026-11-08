#include "media/HttpTsEpollDispatcher.h"
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <cassert>
#include <atomic>
#include <array>
#include <chrono>
#include <iostream>
#include <vector>
#include <thread>
using dvbstreamer5::media::network::HttpTsEpollDispatcher;
struct Pair{int producer=-1,receiver=-1;};
int main(){
    std::atomic<int> closed{0};
    std::vector<Pair> pairs;
    {
        HttpTsEpollDispatcher dispatcher(4);
        for(int i=0;i<24;++i){
            int media[2]={-1,-1},network[2]={-1,-1};
            assert(::socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,media)==0);
            assert(::socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,network)==0);
            const int fd=network[0],hubFd=media[0];
            assert(dispatcher.add(fd,hubFd,[&closed,fd,hubFd](
                const char*,std::uint64_t){
                ::close(fd);::close(hubFd);
                ++closed;
            }));
            struct timeval recvTimeout{3,0};
            ::setsockopt(network[1],SOL_SOCKET,SO_RCVTIMEO,
                         &recvTimeout,sizeof(recvTimeout));
            pairs.push_back({media[1],network[1]});
        }
        std::array<char,1316> input{};
        for(std::size_t i=0;i<input.size();++i)
            input[i]=static_cast<char>((i*31+7)&255);
        for(const auto& pair:pairs){
            assert(::send(pair.producer,input.data(),input.size(),0)==
                   static_cast<ssize_t>(input.size()));
        }
        for(const auto& pair:pairs){
            std::array<char,1316> output{};
            std::size_t offset=0;
            while(offset<output.size()){
                const auto n=::recv(pair.receiver,output.data()+offset,
                                    output.size()-offset,0);
                assert(n>0);
                offset+=static_cast<std::size_t>(n);
            }
            assert(output==input);
        }
        // Token revocation must close only that one HTTP viewer. The
        // epoll worker also notices revoked sessions on idle streams.
        int revocableMedia[2]={-1,-1},revocableNetwork[2]={-1,-1};
        assert(::socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,revocableMedia)==0);
        assert(::socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,revocableNetwork)==0);
        std::atomic<bool> authorized{true};
        std::atomic<int> revoked{0};
        assert(dispatcher.add(revocableNetwork[0],revocableMedia[0],
            [&](const char* reason,std::uint64_t){
                if(std::string(reason)=="middleware_session_revoked")++revoked;
                ::close(revocableNetwork[0]);
                ::close(revocableMedia[0]);
                ++closed;
            },
            [&]{return authorized.load(std::memory_order_acquire);}));
        authorized.store(false,std::memory_order_release);
        for(int wait=0;wait<35 && !revoked.load();++wait)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        assert(revoked.load()==1);
        ::close(revocableMedia[1]);
        ::close(revocableNetwork[1]);
        for(auto& pair:pairs){
            ::close(pair.producer);
            ::close(pair.receiver);
        }
        dispatcher.stop();
    }
    assert(closed==static_cast<int>(pairs.size())+1);
    std::cout<<"HTTP TS shared epoll delivery: OK, clients="<<closed<<"\n";
}
