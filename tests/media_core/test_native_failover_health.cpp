#include "media/NativeFailoverHealth.h"
#include <cstdint>
#include <stdexcept>
#include <iostream>
static void require(bool b) { if(!b) throw std::runtime_error("failback policy failed"); }
int main() {
    using namespace dvbstreamer5::media::mpegts;
    Packet pat{},video{},audio{},nullPacket{};
    auto prepare=[](Packet& p,std::uint16_t pid,bool start){
        p.fill(0xff);p[0]=0x47;
        p[1]=static_cast<std::uint8_t>(((pid>>8)&0x1fU)|(start?0x40U:0U));
        p[2]=static_cast<std::uint8_t>(pid);p[3]=0x10;
    };
    prepare(pat,0,true);pat[4]=0;pat[5]=0;
    prepare(nullPacket,0x1fff,false);
    prepare(video,0x100,true); video[4]=0;video[5]=0;video[6]=1;video[7]=0xe0;
    prepare(audio,0x101,true); audio[4]=0;audio[5]=0;audio[6]=1;audio[7]=0xc0;
    require(!isPlayablePesStart(pat.data(),pat.size()));
    require(!isPlayablePesStart(nullPacket.data(),nullPacket.size()));
    require(isPlayablePesStart(video.data(),video.size()));
    require(isPlayablePesStart(audio.data(),audio.size()));
    video[0]=0;require(!isPlayablePesStart(video.data(),video.size()));
    unsigned n=0;
    n=successivePrimaryMediaChecks(n,true);require(n==1&&!primaryMediaRestored(n));
    n=successivePrimaryMediaChecks(n,false);require(n==0);
    n=successivePrimaryMediaChecks(n,true);require(!primaryMediaRestored(n));
    n=successivePrimaryMediaChecks(n,true);require(primaryMediaRestored(n));
    n=successivePrimaryMediaChecks(n,true);require(n==2);
    std::cout<<"native primary failback health OK\n";
}
