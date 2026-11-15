#include "media/T2miInnerTs.h"
#ifdef NDEBUG
#undef NDEBUG // Keep fixture assertions active in Release GitHub Actions.
#endif
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
static std::vector<Packet> fixture(int plp,bool corrupt=false,int firstCc=0){
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
    int cc=firstCc;
    while(p<mi.size()){
        Packet ts{};ts.fill(0xff);
        ts[0]=0x47;ts[1]=0x10;ts[2]=0x00;
        const bool first=p==0;
        if(first)ts[1]|=0x40;
        ts[3]=static_cast<std::uint8_t>((cc++)&15);
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

    // Multiple simulated PLPs share a single continuous outer TS PID.
    // Only PLP 7 may reach this decoder; PLP 8 must be observable but absent.
    T2miInnerTs multiplex(4096,7,0);
    result.clear();
    int nextCc=0;
    for(int frame=0;frame<20;++frame){
        const int plp=frame%2==0 ? 8 : 7;
        const auto packets=fixture(plp,false,nextCc);
        nextCc=(nextCc+static_cast<int>(packets.size()))&15;
        for(const auto& pkt:packets)multiplex.feed(pkt,result);
    }
    assert(result.size()==10);
    assert(multiplex.stats().miPackets==20);
    assert(multiplex.stats().bbframes==10);
    assert(multiplex.stats().plpFrames[7]==10);
    assert(multiplex.stats().plpFrames[8]==10);
    assert(multiplex.stats().ccErrors==0);
    for(const auto& pkt:result)assert(pkt[0]==0x47);

    // Outer PID isolation: foreign packets never reach the extractor.
    auto foreign=fixture(7);
    for(auto& pkt:foreign)pkt[2]=0x01;
    T2miInnerTs isolated(4096,7,0);
    result.clear();
    for(const auto& pkt:foreign)isolated.feed(pkt,result);
    assert(isolated.stats().outerPackets==0 && result.empty());

    // Emulated missing outer packet causes a CC gap: no partial TS output.
    auto lost=fixture(7);
    assert(lost.size()>=2);
    lost[1][3]=static_cast<std::uint8_t>((lost[1][3]&0xf0)|9);
    T2miInnerTs damaged(4096,7,0);
    result.clear();
    for(const auto& pkt:lost)damaged.feed(pkt,result);
    assert(result.empty() && damaged.stats().ccErrors==1);

    // Auto PLP discovery should lock to the first successfully decoded PLP.
    T2miInnerTs automatic(4096,-1,0);
    result.clear();
    for(const auto& pkt:fixture(11))automatic.feed(pkt,result);
    assert(automatic.activePlp()==11 && result.size()==1);
    return 0;
}
