#pragma once

#include "media/PcrVbrSourceClock.h"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <utility>

namespace dvbstreamer5::media::mpegts {

// V609: opt-in, passive timestamp audit at the exact same MPEG-TS boundaries
// as the V608 CC traces. Never changes PCR/PTS/DTS/CC or packet ordering.
class PcrStageTrace {
public:
    PcrStageTrace(std::string streamId,std::string stage)
        :streamId_(std::move(streamId)),stage_(std::move(stage)) {}

    void inspect(const std::uint8_t* data,std::size_t size) {
        if(!data || size==0 || size%kPacketSize!=0)return;
        for(std::size_t offset=0;offset<size;offset+=kPacketSize,++index_) {
            const auto* packet=data+offset;
            if(packet[0]!=0x47 || (packet[3]&0x20U)==0 ||
               packet[4]<7 || (packet[5]&0x10U)==0)continue;
            Packet copy{};
            std::memcpy(copy.data(),packet,kPacketSize);
            const bool reset=clock_.observe(copy,index_);
            if(!tracking_ && clock_.referencePid()!=0x1fffU) {
                tracking_=true;
                std::cerr<<"PCR STAGE TRACK stream="<<streamId_
                         <<" stage="<<stage_
                         <<" pcr_pid="<<clock_.referencePid()
                         <<std::endl;
            }
            if(reset){
                std::cerr<<"PCR STAGE RESET stream="<<streamId_
                         <<" stage="<<stage_
                         <<" cause="<<clock_.lastResetCause()
                         <<" pcr_pid="<<clock_.referencePid()
                         <<" pcr_jump_ms="<<clock_.lastResetDeltaMs()
                         <<" pcr_wall_gap_ms="<<clock_.lastResetWallGapMs()
                         <<" packet_gap="<<clock_.lastResetPacketGap()
                         <<" prev_packet_index="<<clock_.lastResetPreviousIndex()
                         <<" packet_index="<<clock_.lastResetCurrentIndex()
                         <<std::endl;
            }
        }
    }
private:
    static constexpr std::size_t kPacketSize=188;
    std::string streamId_,stage_;
    PcrVbrSourceClock clock_;
    std::uint64_t index_=0;
    bool tracking_=false;
};

} // namespace dvbstreamer5::media::mpegts
