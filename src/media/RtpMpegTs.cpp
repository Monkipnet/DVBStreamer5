#include "media/RtpMpegTs.h"

#include <algorithm>
#include <stdexcept>

namespace tvs::media::rtp {
namespace {

std::uint16_t read16(const std::uint8_t* data) noexcept {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(data[0]) << 8) | data[1]);
}

std::uint32_t read32(const std::uint8_t* data) noexcept {
    return (static_cast<std::uint32_t>(data[0]) << 24) |
           (static_cast<std::uint32_t>(data[1]) << 16) |
           (static_cast<std::uint32_t>(data[2]) << 8) |
           static_cast<std::uint32_t>(data[3]);
}

void write16(std::uint8_t* data, std::uint16_t value) noexcept {
    data[0] = static_cast<std::uint8_t>(value >> 8);
    data[1] = static_cast<std::uint8_t>(value);
}

void write32(std::uint8_t* data, std::uint32_t value) noexcept {
    data[0] = static_cast<std::uint8_t>(value >> 24);
    data[1] = static_cast<std::uint8_t>(value >> 16);
    data[2] = static_cast<std::uint8_t>(value >> 8);
    data[3] = static_cast<std::uint8_t>(value);
}

} // namespace

bool parsePacket(const std::uint8_t* data, std::size_t size, PacketView& packet) noexcept {
    if (!data || size < 12 || (data[0] >> 6) != 2) {
        return false;
    }

    std::size_t headerSize = 12 + static_cast<std::size_t>(data[0] & 0x0fU) * 4;
    if (headerSize > size) {
        return false;
    }

    if ((data[0] & 0x10U) != 0) {
        if (size - headerSize < 4) {
            return false;
        }
        const std::size_t extensionSize =
            4 + static_cast<std::size_t>(read16(data + headerSize + 2)) * 4;
        if (extensionSize > size - headerSize) {
            return false;
        }
        headerSize += extensionSize;
    }

    std::size_t payloadEnd = size;
    if ((data[0] & 0x20U) != 0) {
        const std::size_t paddingSize = data[size - 1];
        if (paddingSize == 0 || paddingSize > size - headerSize) {
            return false;
        }
        payloadEnd -= paddingSize;
    }
    if (payloadEnd <= headerSize) {
        return false;
    }

    PacketView parsed;
    parsed.payloadType = static_cast<std::uint8_t>(data[1] & 0x7fU);
    parsed.marker = (data[1] & 0x80U) != 0;
    parsed.sequenceNumber = read16(data + 2);
    parsed.timestamp = read32(data + 4);
    parsed.sourceId = read32(data + 8);
    parsed.payload = data + headerSize;
    parsed.payloadSize = payloadEnd - headerSize;
    packet = parsed;
    return true;
}

bool decodeMpegTsPayload(
    const PacketView& rtp, std::vector<mpegts::Packet>& packets) {
    if (!rtp.payload || rtp.payloadSize < mpegts::kPacketSize) {
        return false;
    }

    // Match GStreamer's RTP/MP2T depayloader: RFC 2250 carries complete
    // 188-byte TS packets, so discard any incomplete trailing bytes.
    const std::size_t usableSize =
        rtp.payloadSize - (rtp.payloadSize % mpegts::kPacketSize);
    std::vector<mpegts::Packet> decoded;
    decoded.reserve(usableSize / mpegts::kPacketSize);
    for (std::size_t offset = 0; offset < usableSize; offset += mpegts::kPacketSize) {
        if (rtp.payload[offset] != mpegts::kSyncByte) {
            return false;
        }
        mpegts::Packet packet {};
        std::copy_n(rtp.payload + offset, mpegts::kPacketSize, packet.begin());
        decoded.push_back(packet);
    }
    packets.swap(decoded);
    return true;
}

MpegTsPacketizer::MpegTsPacketizer(
    std::uint32_t sourceId,
    std::uint16_t initialSequence,
    std::uint8_t payloadType,
    std::size_t packetsPerDatagram)
    : sourceId_(sourceId),
      sequenceNumber_(initialSequence),
      payloadType_(payloadType),
      packetsPerDatagram_(packetsPerDatagram) {
    if (payloadType > 127 || packetsPerDatagram == 0 || packetsPerDatagram > 7) {
        throw std::invalid_argument("invalid RTP MPEG-TS packetizer settings");
    }
}

bool MpegTsPacketizer::packetize(
    const std::vector<mpegts::Packet>& packets,
    std::uint32_t timestamp,
    std::vector<std::vector<std::uint8_t>>& datagrams) {
    if (packets.empty()) {
        return false;
    }
    for (const auto& packet : packets) {
        if (packet[0] != mpegts::kSyncByte) {
            return false;
        }
    }

    std::vector<std::vector<std::uint8_t>> encoded;
    encoded.reserve((packets.size() + packetsPerDatagram_ - 1) / packetsPerDatagram_);
    std::uint16_t nextSequence = sequenceNumber_;
    for (std::size_t first = 0; first < packets.size(); first += packetsPerDatagram_) {
        const std::size_t count = std::min(packetsPerDatagram_, packets.size() - first);
        std::vector<std::uint8_t> datagram(12 + count * mpegts::kPacketSize);
        datagram[0] = 0x80;
        datagram[1] = payloadType_;
        write16(datagram.data() + 2, nextSequence++);
        write32(datagram.data() + 4, timestamp);
        write32(datagram.data() + 8, sourceId_);
        for (std::size_t i = 0; i < count; ++i) {
            std::copy(
                packets[first + i].begin(),
                packets[first + i].end(),
                datagram.begin() + static_cast<std::ptrdiff_t>(12 + i * mpegts::kPacketSize));
        }
        encoded.push_back(std::move(datagram));
    }

    sequenceNumber_ = nextSequence;
    datagrams.swap(encoded);
    return true;
}

} // namespace tvs::media::rtp
