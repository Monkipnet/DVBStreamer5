#include "media/PcrVbrUdpOutputWorker.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

using namespace dvbstreamer5::media;
using namespace dvbstreamer5::media::network;
static void check(bool condition,const char* message){
    if(!condition){std::cerr<<"FAIL: "<<message<<'\n';std::exit(1);}
}
static mpegts::Packet packet(std::size_t index,std::uint64_t pcrOffset=0){
    mpegts::Packet p{};
    p[0]=0x47;p[1]=0x01;p[2]=0x00;
    p[3]=static_cast<std::uint8_t>(0x10|(index&15U));
    p[12]=static_cast<std::uint8_t>(index&255U);
    if(index%50==0){
        p[3]=static_cast<std::uint8_t>(0x30|(index&15U));
        p[4]=7;p[5]=0x10;
        const std::uint64_t pcr=(index/50ULL)*1350000ULL+pcrOffset;
        const std::uint64_t base=pcr/300,ext=pcr%300;
        p[6]=static_cast<std::uint8_t>(base>>25);
        p[7]=static_cast<std::uint8_t>(base>>17);
        p[8]=static_cast<std::uint8_t>(base>>9);
        p[9]=static_cast<std::uint8_t>(base>>1);
        p[10]=static_cast<std::uint8_t>((base&1)<<7|(ext>>8)&1);
        p[11]=static_cast<std::uint8_t>(ext);
    }
    return p;
}
int main(){
    std::vector<mpegts::Packet> data;
    for(std::size_t i=0;i<500;++i)data.push_back(packet(i));
    std::atomic<std::size_t> received{0};
    std::atomic<bool> invalid{false};
    std::atomic<bool> fault{false};
    {
        PcrVbrUdpOutputWorker worker(nullptr,0,nullptr,nullptr,
            [&](const std::string&){fault=true;},"vbr-shared-test",
            [&](const std::uint8_t* bytes,std::size_t size){
                if(size==0||size%188!=0)invalid=true;
                const auto before=received.fetch_add(size/188);
                for(std::size_t i=0;i<size/188;++i){
                    const auto* p=bytes+i*188;
                    if(p[0]!=0x47||p[1]!=0x01||p[2]!=0x00||
                       p[12]!=static_cast<std::uint8_t>((before+i)&255U))
                        invalid=true;
                }
            });
        check(worker.enqueue(data),"VBR fanout failed to enqueue");
        for(int i=0;i<100&&received.load()<data.size();++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        worker.stop();
    }
    check(!fault,"VBR observer signalled an error");
    check(!invalid,"VBR shared observer corrupted/reordered TS packets");
    check(received==data.size(),"VBR shared observer lost/duplicated packets");
    // V607 home2 regression: two valid source-PCR epochs with an
    // unflagged +10s jump must both reach UDP/HTTP/HLS intact, in order,
    // without a TS packet being deleted merely because the clock jumped.
    {
        std::vector<mpegts::Packet> jumped;
        for(std::size_t i=0;i<600;++i)
            jumped.push_back(packet(i,i>=300?270000000ULL:0ULL));
        std::atomic<std::size_t> delivered{0};
        std::atomic<bool> corruption{false};
        PcrVbrUdpOutputWorker worker(nullptr,2,nullptr,nullptr,
            [&](const std::string&){fault=true;},"v607-forward-jump",
            [&](const std::uint8_t* bytes,std::size_t size){
                const auto start=delivered.fetch_add(size/188);
                if(size==0 || size%188!=0)corruption=true;
                for(std::size_t i=0;i<size/188;++i){
                    const auto* p=bytes+i*188;
                    if(p[0]!=0x47 || p[1]!=0x01 || p[2]!=0x00 ||
                       p[12]!=static_cast<std::uint8_t>((start+i)&255U))
                        corruption=true;
                }
            });
        check(worker.enqueue(jumped),"V607 PCR rollover enqueue failed");
        for(int i=0;i<140&&delivered.load()<jumped.size();++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        check(delivered==jumped.size(),
              "V607 PCR forward jump deleted or stalled queued TS packets");
        worker.stop();
        check(!corruption && !fault,
              "V607 forward jump corrupted or reordered PES/TS payload");
    }
    // A 1500ms HLS callback cannot delay the physical UDP output.
    {
        UdpSocket receiver,sender;
        std::string error;
        check(receiver.openReceiver("127.0.0.1",0,"","",0,error),
              "loopback UDP receiver unavailable");
        check(sender.openSender("127.0.0.1",receiver.localPort(),"",error),
              "loopback UDP sender unavailable");
        PcrVbrUdpOutputWorker worker(&sender,1,nullptr,nullptr,
            [&](const std::string&){fault=true;},"vbr-isolated-udp-test",
            [&](const std::uint8_t*,std::size_t){
                std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            });
        check(worker.enqueue(std::vector<mpegts::Packet>{packet(0)}),
              "single packet enqueue failed");
        std::array<std::uint8_t,1500> datagram{};
        std::size_t n=0;
        check(receiver.receive(datagram.data(),datagram.size(),n,1300,error)&&n==188,
              "slow HLS observer blocked physical UDP sender");
        worker.stop();
        check(!fault,"slow observer callback reported a transport fault");
    }
    std::cout<<"Shared VBR fanout: OK ("<<received<<" packets)\n";
}
