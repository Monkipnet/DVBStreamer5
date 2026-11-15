#include "media/T2miInnerTs.h"
#include <cassert>
#include <cstdint>
#include <vector>
using namespace dvbstreamer5::media::mpegts;
static std::uint32_t crc32(const std::vector<std::uint8_t>& data){
    std::uint32_t crc=0xffffffffU;
    for(auto b:data){crc^=static_cast<std::uint32_t>(b)<<24;
        for(int i=0;i<8;++i)crc=(crc&0x80000000U)?(crc<<1U)^0x04c11db7U:crc<<1U;
    }
    return crc;
}
static std::vector<Packet> fixture(int plp,bool corrupt=false){
    std::vector<std::uint8_t> payload(3+10+188,0);
    payload[1]=static_cast<std::uint8_t>(plp);
    std::uint8_t* bb=payload.data()+3;
    bb[0]=0xc0; // Transport Stream, NM, no NPD/ISSY
    bb[2]=0x05;bb[3]=0xe0; // UPL 188 bytes in bits
    bb[4]=0x05;bb[5]=0xe0; // DFL 188 bytes in bits
    bb[6]=0x47; // SYNC
    // SYNCD=0; first UP is aligned to the start of the BB data field.
    std::uint8_t* user=bb+10;
    user[0]=0; // NM CRC byte replacing TS sync
    user[1]=0x40;user[2]=0;user[3]=0x10;
    for(std::size_t i=4;i<188;++i)user[i]=static_cast<std::uint8_t>(i);
    std::vector<std::uint8_t> mi(6,0);
    const unsigned bits=static_cast<unsigned>(payload.size()*8);
    mi[4]=static_cast<std::uint8_t>(bits>>8);
    mi[5]=static_cast<std::uint8_t>(bits);
    mi.insert(mi.end(),payload.begin(),payload.end());
    const auto crc=crc32(mi);
    mi.push_back(crc>>24);mi.push_back(crc>>16);mi.push_back(crc>>8);mi.push_back(crc);
    if(corrupt)mi[20]^=0x01;
    std::vector<Packet> packets;
    std::size_t p=0;
    int cc=0;
    while(p<mi.size()){
        Packet ts{};ts.fill(0xff);
        ts[0]=0x47;ts[1]=0x10;ts[2]=0x00;
        const bool first=p==0;
        if(first)ts[1]|=0x40;
        ts[3]=static_cast<std::uint8_t>(cc++);
        const std::size_t remain=mi.size()-p;
        const std::size_t dataCount=std::min<std::size_t>(remain,first?183:184);
        std::size_t at=4;
        if(dataCount<(first?183U:184U)){
            ts[3]|=0x30;
            const std::size_t aflen=183-(dataCount+(first?1:0));
            ts[at++]=static_cast<std::uint8_t>(aflen);
            for(std::size_t i=0;i<aflen;++i)ts[at++]=i==0?0x00:0xff;
        }else ts[3]|=0x10;
        if(first)ts[at++]=0; // T2-MI PUSI pointer
        for(std::size_t i=0;i<dataCount;++i)ts[at++]=mi[p+i];
        assert(at==188);
        packets.push_back(ts);p+=dataCount;
    }
    return packets;
}
int main(){
    T2miInnerTs decoder(4096,7,0);
    std::vector<Packet> result;
    for(const auto& ts:fixture(7))decoder.feed(ts,result);
    assert(result.size()==1);
    assert(result[0][0]==0x47 && result[0][1]==0x40);
    assert(decoder.stats().bbframes==1 && decoder.stats().crcErrors==0);
    assert(decoder.stats().plpFrames[7]==1);
    decoder.reset();result.clear();
    for(const auto& ts:fixture(8))decoder.feed(ts,result);
    assert(result.empty()); // PLP isolation
    decoder.reset();result.clear();
    for(const auto& ts:fixture(7,true))decoder.feed(ts,result);
    assert(result.empty() && decoder.stats().crcErrors==1);
    return 0;
}
