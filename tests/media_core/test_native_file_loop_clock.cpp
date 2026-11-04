#include "media/NativeFileLoopClock.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <iostream>
using namespace dvbstreamer5::media::mpegts;
static Packet makePacket(std::uint64_t pcr,unsigned cc) {
 Packet p{};p.fill(0xff);
 p[0]=0x47;p[1]=0x41;p[2]=0;p[3]=static_cast<unsigned char>(0x30 | cc);
 p[4]=7;p[5]=0x10;
 p[6]=pcr>>25;p[7]=pcr>>17;p[8]=pcr>>9;p[9]=pcr>>1;
 p[10]=static_cast<unsigned char>(((pcr&1)<<7)|0x7e);p[11]=0;
 p[12]=0;p[13]=0;p[14]=1;p[15]=0xe0;p[18]=0x80;p[19]=0x80;p[20]=5;
 const auto pts=pcr+45000;
 p[21]=static_cast<unsigned char>(0x21|(((pts>>30)&7)<<1));
 p[22]=pts>>22;p[23]=static_cast<unsigned char>((((pts>>15)&127)<<1)|1);
 p[24]=pts>>7;p[25]=static_cast<unsigned char>(((pts&127)<<1)|1);
 return p;
}
int main(){
 NativeFileLoopClock clock;
 auto p1=makePacket(90000,0),p2=makePacket(180000,1);
 clock.apply(p1);clock.apply(p2);
 clock.nextLoop();
 auto p3=makePacket(90000,0),p4=makePacket(180000,1);
 clock.apply(p3);clock.apply(p4);
 PacketInfo a{},b{},c{},d{};
 assert(inspectPacket(p1.data(),188,a)&&inspectPacket(p2.data(),188,b));
 assert(inspectPacket(p3.data(),188,c)&&inspectPacket(p4.data(),188,d));
 assert(a.pcrBase90k==90000 && b.pcrBase90k==180000);
 assert(c.pcrBase90k>b.pcrBase90k && d.pcrBase90k>c.pcrBase90k);
 assert((p3[3]&15)==2 && (p4[3]&15)==3);
 assert(clock.loops()==1);
 std::cout<<"file loop PCR CC stable\n";
}
