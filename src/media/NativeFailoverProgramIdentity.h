#pragma once
// Output primary MPEG-TS identity: only used when explicit input backup is
// configured. Captures selected SID/PMT/video/audio PIDs from the final PRIMARY
// TS (after remap/transcode) without altering primary output.
#include "media/TransportStream.h"
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>
namespace dvbstreamer5::media::mpegts {
struct NativeFailoverProgramSnapshot {
    std::uint16_t serviceId=0;
    std::uint16_t pmtPid=0;
    std::uint16_t videoPid=0;
    std::uint16_t audioPid=0;
    bool hasProgram()const noexcept {
        return serviceId!=0 && pmtPid>=0x20U && pmtPid<kNullPid;
    }
};
class NativeFailoverProgramIdentity {
public:
    NativeFailoverProgramSnapshot snapshot()const {
        std::lock_guard<std::mutex> lock(mutex_);
        return state_;
    }
    void observe(const std::vector<Packet>& packets){
        std::lock_guard<std::mutex> lock(mutex_);
        if(state_.hasProgram() && state_.videoPid && state_.audioPid)return;
        for(const auto& p:packets){
            PacketInfo info{};
            if(!inspectPacket(p.data(),p.size(),info) || !info.hasPayload ||
               !info.payloadUnitStart || info.payloadOffset+4U>kPacketSize)continue;
            std::size_t off=info.payloadOffset;
            const std::size_t pointer=p[off++];
            if(pointer>kPacketSize-off||kPacketSize-off-pointer<3U)continue;
            off+=pointer;
            const std::size_t len=((static_cast<std::size_t>(p[off+1]&15U)<<8U)|p[off+2]);
            if(len<9U||len>kPacketSize-off-3U)continue;
            const std::size_t end=off+3U+len-4U;
            if(info.pid==0 && p[off]==0x00U && end>=off+12U){
                for(std::size_t pos=off+8U;pos+4U<=end;pos+=4U){
                    const auto sid=static_cast<std::uint16_t>((p[pos]<<8U)|p[pos+1]);
                    const auto pid=static_cast<std::uint16_t>(((p[pos+2]&31U)<<8U)|p[pos+3]);
                    if(sid && pid>=0x20U && pid<kNullPid){
                        if(state_.serviceId!=sid||state_.pmtPid!=pid){
                            state_.serviceId=sid;state_.pmtPid=pid;
                            state_.videoPid=state_.audioPid=0;
                        }
                        break;
                    }
                }
            }else if(state_.hasProgram() && info.pid==state_.pmtPid &&
                p[off]==0x02U && end>=off+12U &&
                static_cast<std::uint16_t>((p[off+3]<<8U)|p[off+4])==state_.serviceId){
                const std::size_t headerExtra=((static_cast<std::size_t>(p[off+10]&15U)<<8U)|p[off+11]);
                if(headerExtra>end-(off+12U))continue;
                std::size_t pos=off+12U+headerExtra;
                while(pos+5U<=end){
                    const auto type=p[pos];
                    const auto pid=static_cast<std::uint16_t>(((p[pos+1]&31U)<<8U)|p[pos+2]);
                    const std::size_t extra=((static_cast<std::size_t>(p[pos+3]&15U)<<8U)|p[pos+4]);
                    if(extra>end-pos-5U)break;
                    if(!state_.videoPid && (type==1U||type==2U||type==0x1bU||type==0x24U))
                        state_.videoPid=pid;
                    if(!state_.audioPid && (type==3U||type==4U||type==0x0fU||type==0x11U||type==6U))
                        state_.audioPid=pid;
                    pos+=5U+extra;
                }
            }
        }
    }
private:
    mutable std::mutex mutex_;
    NativeFailoverProgramSnapshot state_{};
};
} // namespace dvbstreamer5::media::mpegts
