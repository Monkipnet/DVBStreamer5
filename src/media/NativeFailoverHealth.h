#pragma once
#include "media/TransportStream.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
// A primary MPEG-TS source is playable only after audio/video PES starts;
// PAT/PMT-only, NULL/PCR-only or ECM packets are not recovery evidence.
namespace dvbstreamer5::media::mpegts {
inline bool isPlayablePesStart(const std::uint8_t* packet, std::size_t size) noexcept {
    PacketInfo info;
    if (!packet || !inspectPacket(packet,size,info) ||
        !info.hasPayload || !info.payloadUnitStart ||
        info.payloadOffset > size || size-info.payloadOffset < 4U) return false;
    const auto* p=packet+info.payloadOffset;
    if (p[0]!=0 || p[1]!=0 || p[2]!=1) return false;
    return (p[3]>=0xc0U && p[3]<=0xefU) || p[3]==0xbdU;
}
inline unsigned successivePrimaryMediaChecks(unsigned count,bool mediaPresent) noexcept {
    return mediaPresent ? std::min<unsigned>(count+(count<2U),2U) : 0U;
}
inline bool primaryMediaRestored(unsigned checks) noexcept { return checks>=2U; }
} // namespace dvbstreamer5::media::mpegts
