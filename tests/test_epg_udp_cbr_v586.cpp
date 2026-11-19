// V586 synthetic WISI receiver: ensure fixed UDP pace, PID 18, NULL padding,
// EIT continuity and complete stoppage; no DVB hardware or network dependency.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../src/EpgUdpOutput.h"
#include <arpa/inet.h>
#include <array>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <iostream>
#include <sys/socket.h>
#include <unistd.h>

Json::Value EpgUdpOutputConfig::toJson() const {
    Json::Value root(Json::objectValue);
    root["id"] = id; root["enabled"] = enabled;
    root["bitrate_kbps"] = bitrateKbps; return root;
}
static Json::Value guide() {
    Json::Value root(Json::objectValue), events(Json::arrayValue), e(Json::objectValue);
    const auto now = static_cast<Json::Int64>(std::time(nullptr));
    e["start_utc"]=now-60; e["end_utc"]=now+3600;
    e["event_id"]=1U; e["title"]="Synthetic WISI EPG";
    events.append(e); root["events"]=events; return root;
}
int main() {
    const int fd=socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC,0);
    assert(fd>=0);
    sockaddr_in addr{};
    addr.sin_family=AF_INET;
    addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    assert(bind(fd,reinterpret_cast<const sockaddr*>(&addr),sizeof(addr))==0);
    socklen_t size=sizeof(addr);
    assert(getsockname(fd,reinterpret_cast<sockaddr*>(&addr),&size)==0);
    timeval timeout{};timeout.tv_usec=35000;
    assert(setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout))==0);
    EpgUdpServiceConfig svc;
    svc.streamId="test";svc.serviceId=101;svc.transportStreamId=202;svc.originalNetworkId=303;
    EpgUdpOutputConfig config;
    config.id="wisi-cbr-test";config.enabled=true;
    config.outputHost="127.0.0.1";config.outputPort=ntohs(addr.sin_port);
    config.bitrateKbps=3000;config.services.push_back(svc);
    EpgUdpOutputManager manager([](const EpgUdpServiceConfig&){return guide();});
    manager.configure({config});
    std::array<unsigned,8> windows{};
    unsigned datagrams=0,eit=0,nulls=0;
    int lastContinuity=-1;
    const auto began=std::chrono::steady_clock::now();
    while(std::chrono::steady_clock::now()-began<std::chrono::milliseconds(720)){
        std::array<std::uint8_t,1316> bytes{};
        const ssize_t got=recv(fd,bytes.data(),bytes.size(),0);
        if(got<0){
            assert(errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR);
            continue;
        }
        assert(got==static_cast<ssize_t>(bytes.size()));
        ++datagrams;
        const auto t=std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now()-began).count();
        if(t>=0&&t<800)++windows[static_cast<size_t>(t/100)];
        for(unsigned i=0;i<7;++i){
            const auto* p=bytes.data()+i*188;
            assert(p[0]==0x47);
            const unsigned pid=((p[1]&31U)<<8U)|p[2];
            if(pid==0x1fffU)++nulls;
            else {
                assert(pid==0x12U);++eit;
                const int cc=p[3]&15;
                if(lastContinuity>=0)assert(cc==((lastContinuity+1)&15));
                lastContinuity=cc;
            }
        }
    }
    manager.configure({});
    unsigned filled=0;
    for(unsigned i=1;i<=5;++i)if(windows[i]>2)++filled;
    assert(datagrams>60&&datagrams<450);
    assert(filled>=4);
    assert(eit>0&&nulls>0);
    // Drain buffered datagrams; after stop the socket must become quiet.
    std::array<std::uint8_t,1316> drain{};
    while(recv(fd,drain.data(),drain.size(),MSG_DONTWAIT)>0){}
    timeval shortWait{};shortWait.tv_usec=20000;
    assert(setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&shortWait,sizeof(shortWait))==0);
    assert(recv(fd,drain.data(),drain.size(),0)<0);
    assert(errno==EAGAIN||errno==EWOULDBLOCK);
    close(fd);
    std::cout<<"PASS WISI UDP CBR "<<datagrams
             <<" datagrams; EIT="<<eit<<" NULL="<<nulls
             <<" windows="<<filled<<"/5; stop=quiet\n";
}
