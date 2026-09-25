#pragma once

#include "media/TransportStream.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace tvs::media::rtp {

struct PacketView {
    std::uint8_t payloadType = 0;
    std::uint16_t sequenceNumber = 0;
    std::uint32_t timestamp = 0;
    std::uint32_t sourceId = 0;
    bool marker = false;
    const std::uint8_t* payload = nullptr;
    std::size_t payloadSize = 0;
};

bool parsePacket(const std::uint8_t* data, std::size_t size, PacketView& packet) noexcept;
bool decodeMpegTsPayload(const PacketView& rtp, std::vector<mpegts::Packet>& packets);

class MpegTsPacketizer {
public:
    explicit MpegTsPacketizer(
        std::uint32_t sourceId,
        std::uint16_t initialSequence = 0,
        std::uint8_t payloadType = 33,
        std::size_t packetsPerDatagram = 7);

    bool packetize(
        const std::vector<mpegts::Packet>& packets,
        std::uint32_t timestamp,
        std::vector<std::vector<std::uint8_t>>& datagrams);

private:
    std::uint32_t sourceId_;
    std::uint16_t sequenceNumber_;
    std::uint8_t payloadType_;
    std::size_t packetsPerDatagram_;
};

} // namespace tvs::media::rtp
