#pragma once
// V10.8.534: Native T2-MI (ETSI TS 102 773) MPEG-TS PID -> BBFRAME ->
// selected PLP MPEG-TS. Bounded buffers, CRC32, CC and PUSI recovery.
// Unsupported GSE/NPD/ISSY configurations fail closed (never emit fake TS).
#include "media/TransportStream.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace dvbstreamer5::media::mpegts {
class T2miInnerTs {
public:
    struct Stats {
        std::uint64_t outerPackets=0, miPackets=0, bbframes=0, innerPackets=0;
        std::uint64_t crcErrors=0, ccErrors=0, invalidFrames=0;
        std::array<std::uint64_t,256> plpFrames{};
    };
    explicit T2miInnerTs(std::uint16_t pid, int plp=-1, int stream=-1)
        : pid_(pid), selectedPlp_(plp), selectedStream_(stream) {}
    const Stats& stats() const noexcept {return stats_;}
    int activePlp() const noexcept {return activePlp_;}
    void reset() noexcept {mi_.clear(); carry_.clear(); synchronized_=false; cc_=-1; activePlp_=-1;}
    void feed(const Packet& packet, std::vector<Packet>& output) {
        if(packet[0]!=0x47 || (((packet[1]&0x1fU)<<8)|packet[2])!=pid_)return;
        ++stats_.outerPackets;
        if(packet[1]&0x80U){++stats_.invalidFrames;reset();return;}
        const unsigned afc=(packet[3]>>4)&3U;
        if(afc==0)return;
        std::size_t pos=4;
        bool discontinuity=false;
        if(afc&2U){
            if(pos>=188)return;
            const unsigned len=packet[pos++];
            if(pos+len>188){++stats_.invalidFrames;reset();return;}
            discontinuity=len>0 && (packet[pos]&0x80U)!=0;
            pos+=len;
        }
        if(!(afc&1U)||pos>=188)return;
        const int cc=packet[3]&15;
        if(cc_>=0 && !discontinuity && cc!=((cc_+1)&15)){
            ++stats_.ccErrors;
            reset();
        }
        cc_=cc;
        if(discontinuity){mi_.clear();carry_.clear();synchronized_=false;}
        const bool pusi=(packet[1]&0x40U)!=0;
        if(pusi){
            const unsigned pointer=packet[pos++];
            if(pos+pointer>188){++stats_.invalidFrames;reset();return;}
            if(synchronized_ && pointer>0)append(packet.data()+pos,pointer,output);
            pos+=pointer;
            // A PUSI points to a new T2-MI packet, even after corrupted input.
            // If the previous packet is incomplete, discard it rather than
            // concatenate two unrelated T2-MI packets.
            if(!mi_.empty()){++stats_.invalidFrames;mi_.clear();}
            synchronized_=true;
        }
        if(synchronized_ && pos<188)append(packet.data()+pos,188-pos,output);
    }
private:
    static std::uint32_t crc32(const std::uint8_t* data,std::size_t len){
        std::uint32_t crc=0xffffffffU;
        for(std::size_t i=0;i<len;++i){
            crc^=static_cast<std::uint32_t>(data[i])<<24U;
            for(int b=0;b<8;++b)
                crc=(crc&0x80000000U)?(crc<<1U)^0x04c11db7U:crc<<1U;
        }
        return crc;
    }
    void append(const std::uint8_t* data,std::size_t len,std::vector<Packet>& out){
        // T2-MI has a six-byte header, bit-counted payload, and CRC32.
        // Cap byte assembly independently of hostile length fields.
        while(len){
            if(mi_.size()<6){
                const std::size_t n=std::min(len,6-mi_.size());
                mi_.insert(mi_.end(),data,data+n);data+=n;len-=n;
                if(mi_.size()<6)return;
            }
            const std::size_t bits=(static_cast<std::size_t>(mi_[4])<<8)|mi_[5];
            const std::size_t total=6+(bits+7)/8+4;
            if(total>8202){++stats_.invalidFrames;mi_.clear();synchronized_=false;return;}
            const std::size_t n=std::min(len,total-mi_.size());
            mi_.insert(mi_.end(),data,data+n);data+=n;len-=n;
            if(mi_.size()!=total)return;
            const std::uint32_t expected=
                (static_cast<std::uint32_t>(mi_[total-4])<<24)|
                (static_cast<std::uint32_t>(mi_[total-3])<<16)|
                (static_cast<std::uint32_t>(mi_[total-2])<<8)|mi_[total-1];
            if(crc32(mi_.data(),total-4)==expected){
                ++stats_.miPackets;
                const int stream=mi_[3]&7;
                if(mi_[0]==0 && (selectedStream_<0 || stream==selectedStream_))
                    decodeBb(mi_.data()+6,(bits+7)/8,out);
            }else{
                ++stats_.crcErrors;
                carry_.clear();
            }
            mi_.clear();
        }
    }
    void decodeBb(const std::uint8_t* data,std::size_t size,std::vector<Packet>& out){
        if(size<13){++stats_.invalidFrames;return;}
        const int plp=data[1];
        ++stats_.plpFrames[plp];
        if(selectedPlp_>=0 && plp!=selectedPlp_)return;
        if(activePlp_<0)activePlp_=plp;
        if(plp!=activePlp_)return;
        ++stats_.bbframes;
        const auto* bb=data+3;
        const std::size_t bytes=size-3;
        const unsigned tsGs=(bb[0]>>6)&3U;
        const bool npd=(bb[0]&4U)!=0;
        const bool issy=(bb[0]&8U)!=0;
        const unsigned upl=(static_cast<unsigned>(bb[2])<<8)|bb[3];
        const unsigned dfl=(static_cast<unsigned>(bb[4])<<8)|bb[5];
        const unsigned syncd=(static_cast<unsigned>(bb[7])<<8)|bb[8];
        const bool hem=upl==0 && bb[6]==0;
        const std::size_t stride=hem?187:188;
        if(tsGs!=3U || npd || issy || (!hem && (upl!=1504 || bb[6]!=0x47)) ||
           (dfl&7U) || (syncd!=0xffffU && (syncd&7U)) ||
           10+(dfl/8U)>bytes){
            ++stats_.invalidFrames;carry_.clear();return;
        }
        const std::uint8_t* field=bb+10;
        const std::size_t fieldSize=dfl/8U;
        const std::size_t first=syncd==0xffffU?fieldSize:std::min<std::size_t>(syncd/8U,fieldSize);
        std::size_t p=0;
        // Bytes before SYNCD finish a user packet from the previous BBFRAME.
        if(!carry_.empty()){
            const std::size_t need=stride-carry_.size();
            const std::size_t n=std::min(need,fieldSize);
            carry_.insert(carry_.end(),field,field+n);
            p=n;
            if(carry_.size()==stride)emit(carry_,hem,out);
        }
        if(syncd==0xffffU){
            // No new UP boundary. Retain a valid continuation only.
            if(!carry_.empty() && p<fieldSize &&
               carry_.size()+fieldSize-p<=stride)
                carry_.insert(carry_.end(),field+p,field+fieldSize);
            return;
        }
        if(first>p){ // Lost predecessor: ignore bytes until first new UP.
            carry_.clear();p=first;
        }else if(p>first){ // Existing partial completed beyond advertised boundary.
            carry_.clear();p=first;
        }
        while(p+stride<=fieldSize){
            std::vector<std::uint8_t> up(field+p,field+p+stride);
            emit(up,hem,out);p+=stride;
        }
        carry_.assign(field+p,field+fieldSize);
    }
    void emit(const std::vector<std::uint8_t>& up,bool hem,std::vector<Packet>& out){
        if(up.size()!=(hem?187U:188U))return;
        Packet packet{};
        packet[0]=0x47;
        std::memcpy(packet.data()+1,up.data()+(hem?0:1),187);
        if(((packet[1]&0x1fU)<<8|packet[2])==0x1fffU)return;
        out.push_back(packet);
        ++stats_.innerPackets;
        carry_.clear();
    }
    std::uint16_t pid_;
    int selectedPlp_,selectedStream_,activePlp_=-1,cc_=-1;
    bool synchronized_=false;
    std::vector<std::uint8_t> mi_,carry_;
    Stats stats_;
};
} // namespace dvbstreamer5::media::mpegts
