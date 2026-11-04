#include "media/NativeSrtSwitchClock.h"
#include "media/TransportStream.h"
#include <cassert>
#include <cstdint>
#include <iostream>

using dvbstreamer5::media::mpegts::Packet;
using dvbstreamer5::media::mpegts::PacketInfo;
using dvbstreamer5::media::mpegts::inspectPacket;
using dvbstreamer5::media::srt::NativeSrtSwitchClock;

static void encodeStamp(std::uint8_t* p, std::uint64_t x) {
    p[0]=static_cast<std::uint8_t>(0x20U|(((x>>30U)&7U)<<1U)|1U);
    p[1]=static_cast<std::uint8_t>(x>>22U);
    p[2]=static_cast<std::uint8_t>((((x>>15U)&127U)<<1U)|1U);
    p[3]=static_cast<std::uint8_t>(x>>7U);
    p[4]=static_cast<std::uint8_t>(((x&127U)<<1U)|1U);
}
static std::uint64_t decodeStamp(const std::uint8_t* p) {
    return (static_cast<std::uint64_t>((p[0]>>1U)&7U)<<30U) |
           (static_cast<std::uint64_t>(p[1])<<22U) |
           (static_cast<std::uint64_t>(p[2]>>1U)<<15U) |
           (static_cast<std::uint64_t>(p[3])<<7U) | (p[4]>>1U);
}
static Packet makePacket(std::uint64_t pcr, std::uint64_t pts, unsigned cc) {
    Packet p{};p.fill(0xff);
    p[0]=0x47;p[1]=0x41;p[2]=0x00;p[3]=static_cast<std::uint8_t>(0x30U|(cc&15U));
    p[4]=7;p[5]=0x10;
    p[6]=static_cast<std::uint8_t>(pcr>>25U);
    p[7]=static_cast<std::uint8_t>(pcr>>17U);
    p[8]=static_cast<std::uint8_t>(pcr>>9U);
    p[9]=static_cast<std::uint8_t>(pcr>>1U);
    p[10]=static_cast<std::uint8_t>(((pcr&1U)<<7U)|0x7eU);
    p[11]=0;
    // PES begins at offset 12.
    p[12]=0;p[13]=0;p[14]=1;p[15]=0xe0;p[16]=0;p[17]=0;
    p[18]=0x80;p[19]=0x80;p[20]=5;
    encodeStamp(p.data()+21,pts);
    return p;
}
static std::uint64_t pcrOf(const Packet& p) {
    PacketInfo info{};
    assert(inspectPacket(p.data(),p.size(),info));
    assert(info.hasPcr);
    return info.pcrBase90k;
}
int main() {
    NativeSrtSwitchClock clock;
    auto a=makePacket(100000,145000,5);
    auto b=makePacket(103600,148600,6);
    clock.apply(a.data(),a.size());
    clock.apply(b.data(),b.size());
    assert((a[3]&15U)==5U && (b[3]&15U)==6U);
    assert(pcrOf(b)==103600);
    assert(decodeStamp(b.data()+21)==148600);
    clock.sourceSwitched();
    auto backup=makePacket(90000,135000,0);
    clock.apply(backup.data(),backup.size());
    assert((backup[5]&0x80U)!=0U); // MPEG-TS decoder reset hint
    assert((backup[3]&15U)==7U); // preserve CC across primary->backup
    assert(pcrOf(backup)>pcrOf(b));
    assert(decodeStamp(backup.data()+21)>decodeStamp(b.data()+21));
    assert(decodeStamp(backup.data()+21)-pcrOf(backup)==45000);
    auto repeat=makePacket(93600,138600,1);
    clock.apply(repeat.data(),repeat.size());
    assert(pcrOf(repeat)>pcrOf(backup));
    assert((repeat[3]&15U)==8U);
    clock.sourceSwitched();
    auto restored=makePacket(1000,46000,2);
    clock.apply(restored.data(),restored.size());
    assert((restored[5]&0x80U)!=0U); // failback also signals change
    assert(pcrOf(restored)>pcrOf(repeat));
    assert(decodeStamp(restored.data()+21)>decodeStamp(repeat.data()+21));
    assert((restored[3]&15U)==9U);
    assert(clock.transitions()==2);
    std::cout<<"native SRT source clock/CC transitions OK\\n";
}
