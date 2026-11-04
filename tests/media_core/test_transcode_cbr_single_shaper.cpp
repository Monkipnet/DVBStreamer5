#include "media/CbrTsPacer.h"
#include "media/NativeMpegTsMux.h"
#include "media/TransportStream.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

using namespace dvbstreamer5::media::mpegts;

static std::uint16_t pidOf(const Packet& p) {
    return static_cast<std::uint16_t>((std::uint16_t(p[1] & 0x1fU) << 8U) | p[2]);
}

int main() {
    // Simulate the V336 production mux for a transcoded CBR channel.
    // Only the shared network output shaper is allowed to insert NULL packets.
    NativeMpegTsMux mux;
    NativeMuxConfig config;
    config.videoPid = 0x100;
    config.audioPid = 0x101;
    config.targetBitrate = 0;
    std::string error;
    assert(mux.initialize(config,error));
    assert(mux.setCodec(ElementaryKind::Video,ElementaryCodec::H264,error));
    assert(mux.setCodec(ElementaryKind::Audio,ElementaryCodec::AacAdts,error));

    const std::array<std::uint8_t,8> h264 {
        0,0,0,1,0x65,0x88,0x84,0x21
    };
    ElementarySample video;
    video.data = h264.data();
    video.size = h264.size();
    video.pts90k = 90000;
    video.dts90k = 90000;
    video.hasPts = video.hasDts = true;
    video.randomAccess = true;
    video.duration90k = 3600;

    std::vector<Packet> muxed;
    assert(mux.write(ElementaryKind::Video,video,muxed,error));
    video.pts90k = video.dts90k = 108000;
    assert(mux.write(ElementaryKind::Video,video,muxed,error));
    // A PCM/AAC callback on the same time line must not cause stuffing either.
    const std::array<std::uint8_t,9> aac {
        0xff,0xf1,0x4c,0x80,0x01,0x3f,0xfc,0,0
    };
    ElementarySample audio;
    audio.data = aac.data();
    audio.size = aac.size();
    audio.hasPts = true;
    audio.pts90k = 110000;
    audio.duration90k = 1920;
    assert(mux.write(ElementaryKind::Audio,audio,muxed,error));
    assert(!muxed.empty());

    std::size_t videoPackets=0;
    std::size_t audioPackets=0;
    std::size_t pcrPackets=0;
    for(const Packet& packet:muxed) {
        assert(packet[0]==0x47);
        assert(pidOf(packet)!=kNullPid); // inner mux did not create NULL
        if(pidOf(packet)==config.videoPid) {
            ++videoPackets;
            PacketInfo info;
            assert(inspectPacket(packet.data(),packet.size(),info));
            if(info.hasPcr) ++pcrPackets;
        }
        if(pidOf(packet)==config.audioPid) ++audioPackets;
    }
    assert(videoPackets>=2 && audioPackets>=1 && pcrPackets>=2);

    for(auto rate : {4000000ULL,4000777ULL}) {
        CbrTsPacer output(rate); // final output shaper owns all stuffing
        for(const auto& p:muxed) assert(output.enqueue(p));
        assert(output.started());
        CbrDatagram datagram{};
        assert(output.emitDatagram(std::chrono::steady_clock::now(),datagram));
        std::size_t useful=0, nullCount=0;
        for(const auto& p:datagram) {
            assert(p[0]==0x47);
            if(pidOf(p)==kNullPid) ++nullCount;
            else ++useful;
        }
        assert(useful>0);
        assert(nullCount>0);
        assert(useful+nullCount==kPacketsPerCbrDatagram);
        assert(output.targetBitrate()==4000000ULL);
    }
    std::cout << "Transcode CBR single-shaper test OK\\n";
    return 0;
}
