#include "media/RtpMpegTs.h"
#include "media/NativeUdpRelay.h"
#include "media/NativePreviewHub.h"
#include "media/CbrTsPacer.h"
#include "media/LinuxDvbInput.h"
#include "media/MpegTsRemapper.h"
#include "media/NativeMpegTsMux.h"
#include "media/UdpSocket.h"

#ifdef NDEBUG
#undef NDEBUG
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <climits>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using dvbstreamer5::media::mpegts::ContinuityStatus;
using dvbstreamer5::media::mpegts::Packet;
using dvbstreamer5::media::mpegts::PacketInfo;

namespace {

Packet packet(std::uint16_t pid, std::uint8_t counter, bool payload = true) {
    Packet result {};
    result.fill(0xff);
    result[0] = 0x47;
    result[1] = static_cast<std::uint8_t>((pid >> 8) & 0x1fU);
    result[2] = static_cast<std::uint8_t>(pid);
    result[3] = static_cast<std::uint8_t>((payload ? 0x10U : 0x20U) | (counter & 0x0fU));
    if (!payload) {
        result[4] = 183;
        result[5] = 0;
    }
    return result;
}

std::uint32_t sectionCrc(const std::uint8_t* data, std::size_t size) {
    std::uint32_t crc = 0xffffffffU;
    for (std::size_t index = 0; index < size; ++index) {
        crc ^= static_cast<std::uint32_t>(data[index]) << 24;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80000000U) ? (crc << 1) ^ 0x04c11db7U : crc << 1;
        }
    }
    return crc;
}

void appendSectionCrc(std::vector<std::uint8_t>& section) {
    const auto crc = sectionCrc(section.data(), section.size());
    section.push_back(static_cast<std::uint8_t>(crc >> 24));
    section.push_back(static_cast<std::uint8_t>(crc >> 16));
    section.push_back(static_cast<std::uint8_t>(crc >> 8));
    section.push_back(static_cast<std::uint8_t>(crc));
}

Packet sectionPacket(std::uint16_t pid, std::uint8_t continuity,
                     const std::vector<std::uint8_t>& section) {
    assert(section.size() <= 183);
    Packet result = packet(pid, continuity);
    result[1] |= 0x40;
    result[4] = 0;
    std::copy(section.begin(), section.end(), result.begin() + 5);
    return result;
}

std::vector<std::uint8_t> makePatSection(std::uint16_t sid, std::uint16_t pmtPid) {
    std::vector<std::uint8_t> section = {
        0x00, 0xb0, 0x0d, 0x12, 0x34, 0xc1, 0x00, 0x00,
        static_cast<std::uint8_t>(sid >> 8), static_cast<std::uint8_t>(sid),
        static_cast<std::uint8_t>(0xe0 | ((pmtPid >> 8) & 0x1f)),
        static_cast<std::uint8_t>(pmtPid)
    };
    appendSectionCrc(section);
    return section;
}

std::vector<std::uint8_t> makePmtSection(
    std::uint16_t sid, std::uint16_t videoPid, std::uint16_t audioPid) {
    std::vector<std::uint8_t> section = {
        0x02, 0xb0, 0x17,
        static_cast<std::uint8_t>(sid >> 8), static_cast<std::uint8_t>(sid),
        0xc1, 0x00, 0x00,
        static_cast<std::uint8_t>(0xe0 | ((videoPid >> 8) & 0x1f)),
        static_cast<std::uint8_t>(videoPid),
        0xf0, 0x00,
        0x02,
        static_cast<std::uint8_t>(0xe0 | ((videoPid >> 8) & 0x1f)),
        static_cast<std::uint8_t>(videoPid), 0xf0, 0x00,
        0x03,
        static_cast<std::uint8_t>(0xe0 | ((audioPid >> 8) & 0x1f)),
        static_cast<std::uint8_t>(audioPid), 0xf0, 0x00
    };
    appendSectionCrc(section);
    return section;
}

std::vector<std::uint8_t> makeCatSection(std::uint16_t emmPid) {
    std::vector<std::uint8_t> section = {
        0x01, 0xb0, 0x0f, 0x00, 0x01, 0xc1, 0x00, 0x00,
        0x09, 0x04, 0x01, 0x00,
        static_cast<std::uint8_t>(0xe0 | ((emmPid >> 8) & 0x1f)),
        static_cast<std::uint8_t>(emmPid)
    };
    appendSectionCrc(section);
    return section;
}

std::uint16_t pidOf(const Packet& packet) {
    return static_cast<std::uint16_t>(
        ((static_cast<std::uint16_t>(packet[1] & 0x1f) << 8) | packet[2]));
}


void testNativeMpegTsMux() {
    using namespace dvbstreamer5::media::mpegts;
    NativeMpegTsMux mux;
    NativeMuxConfig config;
    config.serviceId = 42;
    config.transportStreamId = 7;
    config.originalNetworkId = 9;
    config.pmtPid = 0x1000;
    config.videoPid = 0x0200;
    config.audioPid = 0x0201;
    config.targetBitrate = 4000000;
    config.serviceName = "Native Mux";
    config.serviceProvider = "DVBStreamer5";
    std::string error;
    assert(mux.initialize(config, error));
    assert(mux.setCodec(ElementaryKind::Video, ElementaryCodec::H264, error));
    assert(mux.setCodec(ElementaryKind::Audio, ElementaryCodec::AacAdts, error));

    const std::array<std::uint8_t, 8> video = {0x00,0x00,0x00,0x01,0x65,0x88,0x84,0x21};
    ElementarySample videoSample;
    videoSample.data = video.data();
    videoSample.size = video.size();
    videoSample.pts90k = 90000;
    videoSample.dts90k = 90000;
    videoSample.duration90k = 3600;
    videoSample.hasPts = true;
    videoSample.hasDts = true;
    videoSample.randomAccess = true;
    std::vector<Packet> output;
    assert(mux.write(ElementaryKind::Video, videoSample, output, error));
    assert(output.size() >= 4);
    for (const auto& ts : output) assert(ts[0] == 0x47);
    assert(pidOf(output[0]) == 0x0000);
    assert(pidOf(output[1]) == 0x1000);
    assert(pidOf(output[2]) == 0x0011);

    const auto& pat = output[0];
    assert(sectionCrc(pat.data() + 5, 16) == 0);
    assert(pat[13] == 0x00 && pat[14] == 42);
    assert(((pat[15] & 0x1f) << 8 | pat[16]) == 0x1000);

    const auto& pmt = output[1];
    const std::size_t pmtSectionLength = static_cast<std::size_t>(((pmt[6] & 0x0f) << 8) | pmt[7]);
    assert(sectionCrc(pmt.data() + 5, 3 + pmtSectionLength) == 0);
    assert(pmt[17] == 0x1b);
    assert(((pmt[18] & 0x1f) << 8 | pmt[19]) == 0x0200);
    assert(pmt[22] == 0x0f);
    assert(((pmt[23] & 0x1f) << 8 | pmt[24]) == 0x0201);

    bool sawVideoPes = false;
    bool sawPcr = false;
    for (const auto& ts : output) {
        PacketInfo info;
        assert(inspectPacket(ts.data(), ts.size(), info));
        if (info.pid != 0x0200) continue;
        sawPcr = sawPcr || info.hasPcr;
        if (!info.payloadUnitStart || !info.hasPayload) continue;
        const std::size_t offset = info.payloadOffset;
        assert(offset + 9 < ts.size());
        assert(ts[offset] == 0x00 && ts[offset+1] == 0x00 && ts[offset+2] == 0x01);
        assert(ts[offset+3] == 0xe0);
        sawVideoPes = true;
    }
    assert(sawVideoPes && sawPcr);

    output.clear();
    const std::array<std::uint8_t, 9> aac = {0xff,0xf1,0x4c,0x80,0x01,0x3f,0xfc,0x00,0x00};
    ElementarySample audioSample;
    audioSample.data = aac.data();
    audioSample.size = aac.size();
    audioSample.pts90k = 91920;
    audioSample.duration90k = 1920;
    audioSample.hasPts = true;
    assert(mux.write(ElementaryKind::Audio, audioSample, output, error));
    bool sawAudioPes = false;
    for (const auto& ts : output) {
        PacketInfo info;
        assert(inspectPacket(ts.data(), ts.size(), info));
        if (info.pid == 0x0201 && info.payloadUnitStart && info.hasPayload) {
            const std::size_t offset = info.payloadOffset;
            assert(ts[offset] == 0x00 && ts[offset+1] == 0x00 && ts[offset+2] == 0x01);
            assert(ts[offset+3] == 0xc0);
            sawAudioPes = true;
        }
    }
    assert(sawAudioPes);

    output.clear();
    videoSample.pts90k = 108000;
    videoSample.dts90k = 108000;
    assert(mux.write(ElementaryKind::Video, videoSample, output, error));
    const auto nullCount = std::count_if(output.begin(), output.end(), [](const Packet& ts) {
        return pidOf(ts) == kNullPid;
    });
    assert(nullCount > 0);
}

void testMpegTsRemapper() {
    dvbstreamer5::media::mpegts::Remapper remapper;
    dvbstreamer5::media::mpegts::RemapConfig config;
    config.inputServiceId = 10;
    config.outputServiceId = 42;
    config.outputVideoPid = 0x200;
    config.outputAudioPid = 0x201;
    config.serviceName = "Remapped";
    config.serviceProvider = "DVBStreamer5";
    std::string error;
    assert(remapper.initialize(config, error));

    std::vector<Packet> output;
    assert(remapper.process(packet(0x300, 0), output, error));
    assert(output.empty());
    const auto pat = sectionPacket(0, 3, makePatSection(10, 0x1000));
    assert(remapper.process(pat, output, error));
    assert(output.empty());
    const auto pmt = sectionPacket(0x1000, 5, makePmtSection(10, 0x100, 0x101));
    assert(remapper.process(pmt, output, error));
    assert(output.size() == 2);
    assert(pidOf(output[0]) == 0);
    assert(pidOf(output[1]) == 0x1000);
    assert(output[0][13] == 0 && output[0][14] == 42);
    assert(sectionCrc(output[0].data() + 5, 16) == 0);
    assert(output[1][8] == 0 && output[1][9] == 42);
    assert(output[1][18] == 0xE2 && output[1][19] == 0x00);
    assert(output[1][23] == 0xE2 && output[1][24] == 0x01);
    assert(sectionCrc(output[1].data() + 5, 26) == 0);
    output.clear();

    assert(remapper.process(sectionPacket(0x0001, 0, makeCatSection(0x120)), output, error));
    assert(output.size() == 1 && pidOf(output.front()) == 0x0001);
    output.clear();
    assert(remapper.process(packet(0x0120, 0), output, error));
    assert(output.size() == 1 && pidOf(output.front()) == 0x0120);
    output.clear();

    auto video = packet(0x100, 1);
    assert(remapper.process(video, output, error));
    assert(output.size() == 1 && pidOf(output.front()) == 0x200);
    output.clear();
    assert(remapper.process(packet(0x300, 1), output, error));
    assert(output.empty());
    assert(remapper.process(pat, output, error));
    assert(output.size() == 1 && pidOf(output.front()) == 0);
    assert((output.front()[3] & 0x0f) == 3);
    output.clear();

    auto fragmentedPat = packet(0, 4);
    fragmentedPat[1] |= 0x40;
    fragmentedPat[4] = 0;
    fragmentedPat[5] = 0x00;
    fragmentedPat[6] = 0xb0;
    fragmentedPat[7] = 0xc0;
    assert(!remapper.process(fragmentedPat, output, error));
    assert(error.find("one TS packet") != std::string::npos);
}

class HttpTsTestServer {
public:
    explicit HttpTsTestServer(
        std::vector<std::uint8_t> body,
        int responseCode = 200,
        std::string redirectLocation = {})
        : body_(std::move(body)),
          responseCode_(responseCode),
          redirectLocation_(std::move(redirectLocation)) {
        listener_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        assert(listener_ != kInvalidSocket);
        int reuseAddress = 1;
#ifdef _WIN32
        const int reuseResult = ::setsockopt(
            listener_, SOL_SOCKET, SO_REUSEADDR,
            reinterpret_cast<const char*>(&reuseAddress),
            sizeof(reuseAddress));
#else
        const int reuseResult = ::setsockopt(
            listener_, SOL_SOCKET, SO_REUSEADDR,
            &reuseAddress, sizeof(reuseAddress));
#endif
        assert(reuseResult == 0);
        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        assert(::bind(
            listener_, reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)) == 0);
#ifdef _WIN32
        int addressSize = sizeof(address);
#else
        socklen_t addressSize = sizeof(address);
#endif
        assert(::getsockname(
            listener_, reinterpret_cast<sockaddr*>(&address), &addressSize) == 0);
        port_ = ntohs(address.sin_port);
        assert(::listen(listener_, 1) == 0);
        worker_ = std::thread(&HttpTsTestServer::serve, this);
    }

    ~HttpTsTestServer() {
        closeSocket(listener_);
        join();
    }

    std::uint16_t port() const noexcept {
        return port_;
    }

    void join() {
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    std::string request() const {
        std::lock_guard<std::mutex> lock(requestMutex_);
        return request_;
    }

private:
#ifdef _WIN32
    using Socket = SOCKET;
    static constexpr Socket kInvalidSocket = INVALID_SOCKET;
    static void closeSocket(Socket socket) {
        if (socket != kInvalidSocket) ::closesocket(socket);
    }
#else
    using Socket = int;
    static constexpr Socket kInvalidSocket = -1;
    static void closeSocket(Socket socket) {
        if (socket != kInvalidSocket) ::close(socket);
    }
#endif

    void serve() {
        const Socket client = ::accept(listener_, nullptr, nullptr);
        if (client == kInvalidSocket) {
            return;
        }

        std::string request;
        std::array<char, 4096> buffer {};
        while (request.find("\r\n\r\n") == std::string::npos &&
            request.size() < 16384) {
#ifdef _WIN32
            const int count = ::recv(
                client, buffer.data(), static_cast<int>(buffer.size()), 0);
#else
            const ssize_t count = ::recv(
                client, buffer.data(), buffer.size(), 0);
#endif
            if (count <= 0) {
                break;
            }
            request.append(buffer.data(), static_cast<std::size_t>(count));
        }
        {
            std::lock_guard<std::mutex> lock(requestMutex_);
            request_ = std::move(request);
        }

        const std::string statusLine = responseCode_ == 200
            ? "HTTP/1.1 200 OK\r\n"
            : "HTTP/1.1 " + std::to_string(responseCode_) +
                (responseCode_ == 302 ? " Found\r\n" : " Not Found\r\n");
        const std::string locationHeader = redirectLocation_.empty()
            ? std::string()
            : "Location: " + redirectLocation_ + "\r\n";
        const std::string responseHeaders = statusLine + locationHeader +
            "Content-Type: video/mp2t\r\nContent-Length: " +
            std::to_string(body_.size()) + "\r\nConnection: close\r\n\r\n";
        sendAll(client,
            reinterpret_cast<const std::uint8_t*>(responseHeaders.data()),
            responseHeaders.size());
        if (responseCode_ == 200) {
            sendAll(client, body_.data(), body_.size());
        }
        closeSocket(client);
    }

    static void sendAll(Socket socket, const std::uint8_t* data, std::size_t size) {
        while (size > 0) {
#ifdef _WIN32
            const int sent = ::send(
                socket, reinterpret_cast<const char*>(data),
                static_cast<int>((std::min)(size, static_cast<std::size_t>(INT_MAX))),
                0);
#else
            const ssize_t sent = ::send(socket, data, size, 0);
#endif
            if (sent <= 0) {
                return;
            }
            data += sent;
            size -= static_cast<std::size_t>(sent);
        }
    }

    Socket listener_ = kInvalidSocket;
    std::uint16_t port_ = 0;
    std::vector<std::uint8_t> body_;
    int responseCode_ = 200;
    std::string redirectLocation_;
    mutable std::mutex requestMutex_;
    std::string request_;
    std::thread worker_;
};

void write16(std::uint8_t* data, std::uint16_t value) {
    data[0] = static_cast<std::uint8_t>(value >> 8);
    data[1] = static_cast<std::uint8_t>(value);
}

void write32(std::uint8_t* data, std::uint32_t value) {
    data[0] = static_cast<std::uint8_t>(value >> 24);
    data[1] = static_cast<std::uint8_t>(value >> 16);
    data[2] = static_cast<std::uint8_t>(value >> 8);
    data[3] = static_cast<std::uint8_t>(value);
}

void testPacketInspectionAndPidRewrite() {
    Packet ts = packet(0x0123, 5);
    PacketInfo info;
    assert(dvbstreamer5::media::mpegts::inspectPacket(ts.data(), ts.size(), info));
    assert(info.pid == 0x0123 && info.continuityCounter == 5 && info.hasPayload);
    assert(info.payloadOffset == 4);

    ts[3] = 0x30;
    ts[4] = 7;
    ts[5] = 0x90;
    ts[6] = 0x01;
    ts[7] = 0x02;
    ts[8] = 0x03;
    ts[9] = 0x04;
    ts[10] = 0x05;
    ts[11] = 0x06;
    ts[12] = 0x07;
    assert(dvbstreamer5::media::mpegts::inspectPacket(ts.data(), ts.size(), info));
    assert(info.hasAdaptationField && info.hasPcr && info.discontinuity);
    assert(info.payloadOffset == 12);
    assert(info.pcrBase90k == 33818120);
    assert(dvbstreamer5::media::mpegts::rewritePid(ts.data(), ts.size(), 0x1ffe));
    assert(dvbstreamer5::media::mpegts::inspectPacket(ts.data(), ts.size(), info));
    assert(info.pid == 0x1ffe && info.hasPcr);

    const Packet before = ts;
    assert(!dvbstreamer5::media::mpegts::rewritePid(ts.data(), ts.size(), 0x2000));
    assert(ts == before);
    ts[0] = 0;
    assert(!dvbstreamer5::media::mpegts::inspectPacket(ts.data(), ts.size(), info));
    ts = packet(1, 0);
    ts[3] &= 0x0f;
    assert(!dvbstreamer5::media::mpegts::inspectPacket(ts.data(), ts.size(), info));
    ts = packet(1, 0);
    ts[3] = 0x30;
    ts[4] = 184;
    assert(!dvbstreamer5::media::mpegts::inspectPacket(ts.data(), ts.size(), info));
    ts[4] = 1;
    ts[5] = 0x10;
    assert(!dvbstreamer5::media::mpegts::inspectPacket(ts.data(), ts.size(), info));
}

void testFraming() {
    using dvbstreamer5::media::mpegts::PacketFramer;
    Packet first = packet(10, 0);
    Packet second = packet(11, 1);
    Packet third = packet(12, 2);
    std::vector<std::uint8_t> input {0x00, 0x01, 0x47, 0x02};
    input.insert(input.end(), first.begin(), first.end());
    input.insert(input.end(), second.begin(), second.end());
    input.insert(input.end(), third.begin(), third.end());

    PacketFramer framer;
    std::vector<Packet> output;
    framer.push(input.data(), 73, output);
    assert(output.empty());
    framer.push(input.data() + 73, input.size() - 73, output);
    assert(output.size() == 3);
    assert(output[0] == first && output[1] == second && output[2] == third);

    framer.reset();
    framer.push(input.data(), input.size(), output);
    assert(output.size() == 6);
}

void testContinuityTracking() {
    using dvbstreamer5::media::mpegts::ContinuityTracker;
    ContinuityTracker tracker;
    assert(tracker.observe(packet(100, 14).data(), 188) == ContinuityStatus::InOrder);
    assert(tracker.observe(packet(100, 15).data(), 188) == ContinuityStatus::InOrder);
    assert(tracker.observe(packet(100, 15).data(), 188) == ContinuityStatus::Duplicate);
    assert(tracker.observe(packet(100, 1).data(), 188) == ContinuityStatus::Gap);
    assert(tracker.observe(packet(100, 1, false).data(), 188) == ContinuityStatus::InOrder);
    assert(tracker.observe(packet(100, 2).data(), 188) == ContinuityStatus::InOrder);

    Packet discontinuity = packet(100, 9);
    discontinuity[3] = 0x30 | 9;
    discontinuity[4] = 1;
    discontinuity[5] = 0x80;
    assert(tracker.observe(discontinuity.data(), 188) == ContinuityStatus::Discontinuity);
    assert(tracker.observe(packet(100, 10).data(), 188) == ContinuityStatus::InOrder);
    assert(tracker.observe(packet(0x1fff, 0).data(), 188) == ContinuityStatus::NullPacket);
    Packet transportError = packet(100, 11);
    transportError[1] |= 0x80;
    assert(tracker.observe(transportError.data(), 188) == ContinuityStatus::TransportError);
    assert(tracker.observe(nullptr, 0) == ContinuityStatus::InvalidPacket);
}

void testRtpParsingAndPayload() {
    using dvbstreamer5::media::rtp::PacketView;
    using dvbstreamer5::media::rtp::decodeMpegTsPayload;
    using dvbstreamer5::media::rtp::parsePacket;

    const Packet first = packet(0x100, 0);
    const Packet second = packet(0x101, 1);
    std::vector<std::uint8_t> datagram(12 + 2 * 188);
    datagram[0] = 0x80;
    datagram[1] = 0xe0;
    write16(datagram.data() + 2, 65535);
    write32(datagram.data() + 4, 90000);
    write32(datagram.data() + 8, 0x10203040);
    std::copy(first.begin(), first.end(), datagram.begin() + 12);
    std::copy(second.begin(), second.end(), datagram.begin() + 200);

    PacketView view;
    assert(parsePacket(datagram.data(), datagram.size(), view));
    assert(view.marker && view.payloadType == 96 && view.sequenceNumber == 65535);
    assert(view.timestamp == 90000 && view.sourceId == 0x10203040);
    std::vector<Packet> decoded;
    assert(decodeMpegTsPayload(view, decoded));
    assert(decoded.size() == 2 && decoded[0] == first && decoded[1] == second);

    datagram.push_back(0xaa);
    assert(parsePacket(datagram.data(), datagram.size(), view));
    assert(view.payloadSize == 2 * 188 + 1);
    assert(decodeMpegTsPayload(view, decoded));
    assert(decoded.size() == 2);

    std::vector<std::uint8_t> extended(12 + 4 + 4 + 4 + 188 + 1, 0);
    extended[0] = 0xb1;
    extended[1] = 33;
    write16(extended.data() + 16, 0xbede);
    write16(extended.data() + 18, 1);
    std::copy(first.begin(), first.end(), extended.begin() + 24);
    extended.back() = 1;
    assert(parsePacket(extended.data(), extended.size(), view));
    assert(view.payloadSize == 188 && view.payload[0] == 0x47);

    const std::size_t oldSize = decoded.size();
    extended[24] = 0;
    assert(parsePacket(extended.data(), extended.size(), view));
    assert(!decodeMpegTsPayload(view, decoded));
    assert(decoded.size() == oldSize);
    assert(!parsePacket(datagram.data(), 8, view));
    datagram[0] = 0;
    assert(!parsePacket(datagram.data(), datagram.size(), view));
    std::array<std::uint8_t, 16> truncatedExtension {};
    truncatedExtension[0] = 0x90;
    write16(truncatedExtension.data() + 14, 1);
    assert(!parsePacket(truncatedExtension.data(), truncatedExtension.size(), view));
}

void testRtpPacketizerRoundTrip() {
    using dvbstreamer5::media::rtp::MpegTsPacketizer;
    using dvbstreamer5::media::rtp::PacketView;
    using dvbstreamer5::media::rtp::decodeMpegTsPayload;
    using dvbstreamer5::media::rtp::parsePacket;

    std::vector<Packet> input;
    for (std::uint16_t i = 0; i < 15; ++i) {
        input.push_back(packet(static_cast<std::uint16_t>(300 + i), static_cast<std::uint8_t>(i)));
    }
    MpegTsPacketizer packetizer(0x12345678, 65534, 33, 7);
    std::vector<std::vector<std::uint8_t>> datagrams;
    assert(packetizer.packetize(input, 0x01020304, datagrams));
    assert(datagrams.size() == 3);

    std::vector<Packet> roundTrip;
    for (std::size_t i = 0; i < datagrams.size(); ++i) {
        PacketView view;
        assert(parsePacket(datagrams[i].data(), datagrams[i].size(), view));
        assert(view.payloadType == 33 && !view.marker);
        assert(view.sequenceNumber == static_cast<std::uint16_t>(65534 + i));
        assert(view.timestamp == 0x01020304 && view.sourceId == 0x12345678);
        std::vector<Packet> batch;
        assert(decodeMpegTsPayload(view, batch));
        roundTrip.insert(roundTrip.end(), batch.begin(), batch.end());
    }
    assert(roundTrip == input);

    std::vector<Packet> invalid {input.front()};
    invalid.front()[0] = 0;
    const auto previousOutput = datagrams;
    assert(!packetizer.packetize(invalid, 10, datagrams));
    assert(datagrams == previousOutput);
    assert(!packetizer.packetize({}, 10, datagrams));
    assert(datagrams == previousOutput);
    PacketView next;
    const std::vector<Packet> one {input.front()};
    assert(packetizer.packetize(one, 11, datagrams));
    assert(parsePacket(datagrams.front().data(), datagrams.front().size(), next));
    assert(next.sequenceNumber == 1);

    bool rejected = false;
    try {
        MpegTsPacketizer bad(0, 0, 128, 7);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);
}

void testUdpLoopback() {
    using dvbstreamer5::media::network::UdpSocket;
    UdpSocket receiver;
    UdpSocket sender;
    std::string error;
    assert(receiver.openReceiver("127.0.0.1", 0, "", "", 0, error));
    assert(receiver.localPort() != 0);
    assert(sender.openSender("127.0.0.1", receiver.localPort(), "127.0.0.1", error));

    const Packet expected = packet(0x0100, 4);
    dvbstreamer5::media::rtp::MpegTsPacketizer packetizer(0x55667788);
    std::vector<std::vector<std::uint8_t>> datagrams;
    assert(packetizer.packetize({expected}, 90000, datagrams));
    assert(sender.send(datagrams.front().data(), datagrams.front().size(), error));

    std::array<std::uint8_t, 2048> buffer {};
    std::size_t received = 0;
    assert(receiver.receive(buffer.data(), buffer.size(), received, 2000, error));
    assert(received == datagrams.front().size());
    dvbstreamer5::media::rtp::PacketView rtp;
    assert(dvbstreamer5::media::rtp::parsePacket(buffer.data(), received, rtp));
    assert(rtp.timestamp == 90000 && rtp.sourceId == 0x55667788);
    std::vector<Packet> decoded;
    assert(dvbstreamer5::media::rtp::decodeMpegTsPayload(rtp, decoded));
    assert(decoded.size() == 1 && decoded.front() == expected);

    received = 99;
    assert(receiver.receive(buffer.data(), buffer.size(), received, 5, error));
    assert(received == 0);
    assert(!sender.send(nullptr, 0, error));
}

void testLinuxDvbPidListParsing() {
    using dvbstreamer5::media::network::LinuxDvbInput;
    std::vector<std::uint16_t> pids;
    std::string error;
    assert(LinuxDvbInput::parsePidList("0:17:256:256:8191", pids, error));
    assert((pids == std::vector<std::uint16_t>{0, 17, 256, 8191}));
    assert(error.empty());
    assert(LinuxDvbInput::parsePidList("8192", pids, error));
    assert(pids.empty());
    assert(!LinuxDvbInput::parsePidList("17::256", pids, error));
    assert(pids.empty() && !error.empty());
    assert(!LinuxDvbInput::parsePidList("8192:17", pids, error));
    assert(pids.empty() && !error.empty());
    assert(!LinuxDvbInput::parsePidList("not-a-pid", pids, error));
    assert(pids.empty() && !error.empty());
}

std::uint16_t reserveLocalUdpPort() {
    dvbstreamer5::media::network::UdpSocket socket;
    std::string error;
    assert(socket.openReceiver("127.0.0.1", 0, "", "", 0, error));
    const std::uint16_t port = socket.localPort();
    socket.close();
    return port;
}

void testNativeUdpTsRelay() {
    using namespace dvbstreamer5::media::network;
    const std::uint16_t inputPort = reserveLocalUdpPort();

    UdpSocket outputReceiver;
    UdpSocket inputSender;
    std::string error;
    assert(outputReceiver.openReceiver("127.0.0.1", 0, "", "", 0, error));
    assert(inputSender.openSender("127.0.0.1", inputPort, "", error));

    NativeUdpRelay relay;
    NativeUdpRelayConfig config;
    config.inputUri = "udp://127.0.0.1:" + std::to_string(inputPort);
#if defined(__linux__)
    config.inputInterfaceAddress = "127.0.0.1";
    config.inputInterfaceDeviceName = "lo";
    config.inputInterfaceAddressConfigured = true;
#endif
    config.outputType = "udp-vbr";
    config.outputHost = "127.0.0.1";
    config.outputPort = outputReceiver.localPort();
    std::atomic<unsigned> caCalls{0};
    std::atomic<unsigned> observerCalls{0};
    config.processTransport = [&caCalls](std::uint8_t* data, std::size_t size) {
        assert(size == dvbstreamer5::media::mpegts::kPacketSize);
        data[3] |= 0x80;
        caCalls.fetch_add(1, std::memory_order_relaxed);
        return true;
    };
    config.observeTransport = [&observerCalls](const std::uint8_t* data, std::size_t size) {
        assert(size == dvbstreamer5::media::mpegts::kPacketSize);
        assert((data[3] & 0x80U) != 0);
        observerCalls.fetch_add(1, std::memory_order_relaxed);
    };
    assert(relay.start(config, error));
    assert(relay.isRunning());

    const Packet sourcePacket = packet(0x0137, 8);
    Packet expected = sourcePacket;
    expected[3] |= 0x80;
    assert(inputSender.send(sourcePacket.data(), sourcePacket.size(), error));

    std::array<std::uint8_t, 2048> received {};
    std::size_t size = 0;
    assert(outputReceiver.receive(received.data(), received.size(), size, 2000, error));
    assert(size == expected.size());
    assert(std::equal(expected.begin(), expected.end(), received.begin()));
    assert(caCalls.load(std::memory_order_relaxed) == 1);
    assert(observerCalls.load(std::memory_order_relaxed) == 1);
    const auto counterDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (relay.outputBytes() < expected.size() &&
        std::chrono::steady_clock::now() < counterDeadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(relay.inputBytes() >= expected.size());
    assert(relay.outputBytes() == expected.size());

    relay.stop();
    assert(!relay.isRunning());
    assert(relay.lastError().empty());
}

void testNativeRtpTsRelay() {
    using namespace dvbstreamer5::media::network;
    const std::uint16_t inputPort = reserveLocalUdpPort();

    UdpSocket outputReceiver;
    UdpSocket inputSender;
    std::string error;
    assert(outputReceiver.openReceiver("127.0.0.1", 0, "", "", 0, error));
    assert(inputSender.openSender("127.0.0.1", inputPort, "", error));

    NativeUdpRelay relay;
    NativeUdpRelayConfig config;
    config.inputUri = "rtp://127.0.0.1:" + std::to_string(inputPort);
    config.outputType = "rtp";
    config.outputHost = "127.0.0.1";
    config.outputPort = outputReceiver.localPort();
    assert(relay.start(config, error));

    const Packet expected = packet(0x0237, 9);
    dvbstreamer5::media::rtp::MpegTsPacketizer packetizer(0x76543210, 14);
    std::vector<std::vector<std::uint8_t>> sourceDatagrams;
    assert(packetizer.packetize({expected}, 90000, sourceDatagrams));
    assert(inputSender.send(
        sourceDatagrams.front().data(), sourceDatagrams.front().size(), error));

    std::array<std::uint8_t, 2048> received {};
    std::size_t size = 0;
    assert(outputReceiver.receive(received.data(), received.size(), size, 2000, error));
    dvbstreamer5::media::rtp::PacketView view;
    assert(dvbstreamer5::media::rtp::parsePacket(received.data(), size, view));
    assert(view.payloadType == 33 && view.payloadSize == expected.size());
    std::vector<Packet> decoded;
    assert(dvbstreamer5::media::rtp::decodeMpegTsPayload(view, decoded));
    assert(decoded.size() == 1 && decoded.front() == expected);
    const auto counterDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (relay.outputBytes() < expected.size() &&
        std::chrono::steady_clock::now() < counterDeadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(relay.inputBytes() == sourceDatagrams.front().size());
    assert(relay.outputBytes() == expected.size());
    relay.stop();
}

void testNativeExternallyFedTsRelay() {
    using namespace dvbstreamer5::media::network;

    UdpSocket outputReceiver;
    std::string error;
    assert(outputReceiver.openReceiver("127.0.0.1", 0, "", "", 0, error));

    NativeUdpRelay relay;
    NativeUdpRelayConfig config;
    config.inputUri = "external://hls";
    config.externallyFedInput = true;
    config.outputType = "udp-vbr";
    config.outputHost = "127.0.0.1";
    config.outputPort = outputReceiver.localPort();
    assert(relay.start(config, error));

    const Packet expected = packet(0x0317, 10);
    assert(relay.pushInput(expected.data(), expected.size()));

    std::array<std::uint8_t, 2048> received {};
    std::size_t size = 0;
    assert(outputReceiver.receive(received.data(), received.size(), size, 2000, error));
    assert(size == expected.size());
    assert(std::equal(expected.begin(), expected.end(), received.begin()));

    relay.finishInput();
    relay.stop();
}

void testCbrTsPacer() {
    using namespace dvbstreamer5::media::mpegts;
    const Packet mediaPacket = packet(0x0142, 3);
    CbrTsPacer paddingPacer(2000000);
    assert(paddingPacer.enqueue(mediaPacket));
    CbrDatagram datagram {};
    auto now = std::chrono::steady_clock::now();
    assert(paddingPacer.nextDatagram(now, datagram));
    assert(datagram[0] == mediaPacket);
    PacketInfo info;
    for (std::size_t i = 1; i < datagram.size(); ++i) {
        assert(inspectPacket(datagram[i].data(), datagram[i].size(), info));
        assert(info.pid == kNullPid && info.continuityCounter == i - 1);
    }

    CbrTsPacer pacer(2000000);
    for (std::size_t i = 0; i < 7 * 101; ++i) {
        assert(pacer.enqueue(mediaPacket));
    }
    assert(pacer.started());
    assert(pacer.targetBitrate() == 2000000);
    assert(pacer.nextDatagram(std::chrono::steady_clock::now(), datagram));
    const auto firstDeadline = pacer.nextDeadline();
    assert(!pacer.nextDatagram(firstDeadline - std::chrono::nanoseconds(1), datagram));
    std::uint64_t count = 1;
    while (count < 101) {
        const auto deadline = pacer.nextDeadline();
        assert(pacer.nextDatagram(deadline, datagram));
        ++count;
    }
    const std::uint64_t datagramBits = kPacketsPerCbrDatagram * kPacketSize * 8ULL;
    const std::uint64_t expectedNanoseconds =
        (count - 1) * datagramBits * 1000000000ULL / pacer.targetBitrate();
    const auto actualNanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(
        pacer.nextDeadline() - firstDeadline).count();
    assert(static_cast<std::uint64_t>(actualNanoseconds) == expectedNanoseconds);

    CbrTsPacer clamped(100000);
    assert(clamped.targetBitrate() == 512000);
    bool rejectedZero = false;
    try {
        CbrTsPacer invalid(0);
    } catch (const std::invalid_argument&) {
        rejectedZero = true;
    }
    assert(rejectedZero);

    CbrTsPacer bounded(2000000);
    Packet invalidPacket = mediaPacket;
    invalidPacket[0] = 0;
    assert(!bounded.enqueue(invalidPacket));
    assert(!bounded.started());
    constexpr std::size_t maximumQueuedPackets = (2 * 1024 * 1024) / 188;
    for (std::size_t i = 0; i < maximumQueuedPackets; ++i) {
        assert(bounded.enqueue(mediaPacket));
    }
    assert(bounded.queuedPackets() == maximumQueuedPackets);
    assert(!bounded.enqueue(mediaPacket));
    assert(bounded.queuedPackets() == maximumQueuedPackets);
}

void testNativeUdpCbrRelay() {
    using namespace dvbstreamer5::media::network;
    const std::uint16_t inputPort = reserveLocalUdpPort();

    UdpSocket outputReceiver;
    UdpSocket inputSender;
    std::string error;
    assert(outputReceiver.openReceiver("127.0.0.1", 0, "", "", 0, error));
    assert(inputSender.openSender("127.0.0.1", inputPort, "", error));

    NativeUdpRelay relay;
    NativeUdpRelayConfig config;
    config.inputUri = "udp://127.0.0.1:" + std::to_string(inputPort);
    config.outputType = "udp-cbr";
    config.outputHost = "127.0.0.1";
    config.outputPort = outputReceiver.localPort();
    config.targetBitrate = 512000;
    assert(relay.start(config, error));

    const Packet expected = packet(0x0175, 5);
    assert(inputSender.send(expected.data(), expected.size(), error));

    std::array<std::uint8_t, 2048> received {};
    std::size_t size = 0;
    assert(outputReceiver.receive(received.data(), received.size(), size, 2000, error));
    assert(size == 7 * 188);
    PacketInfo info;
    assert(dvbstreamer5::media::mpegts::inspectPacket(received.data(), 188, info));
    assert(info.pid == 0x0175);
    for (std::size_t i = 1; i < 7; ++i) {
        assert(dvbstreamer5::media::mpegts::inspectPacket(received.data() + i * 188, 188, info));
        assert(info.pid == dvbstreamer5::media::mpegts::kNullPid);
        assert(info.continuityCounter == i - 1);
    }

    const auto firstSentAt = std::chrono::steady_clock::now();
    assert(outputReceiver.receive(received.data(), received.size(), size, 1000, error));
    const auto secondSentAt = std::chrono::steady_clock::now();
    assert(size == 7 * 188);
    assert(secondSentAt - firstSentAt >= std::chrono::milliseconds(15));
    for (std::size_t i = 0; i < 7; ++i) {
        assert(dvbstreamer5::media::mpegts::inspectPacket(received.data() + i * 188, 188, info));
        assert(info.pid == dvbstreamer5::media::mpegts::kNullPid);
        assert(info.continuityCounter == (i + 6) % 16);
    }

    const auto counterDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (relay.outputBytes() < 2 * 7 * 188 &&
        std::chrono::steady_clock::now() < counterDeadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(relay.outputBytes() >= 2 * 7 * 188);
    relay.stop();
}

void testNativeUdpFanoutRelay() {
    using namespace dvbstreamer5::media::network;
    const std::uint16_t inputPort = reserveLocalUdpPort();

    UdpSocket udpReceiver;
    UdpSocket rtpReceiver;
    UdpSocket cbrReceiver;
    UdpSocket inputSender;
    std::string error;
    assert(udpReceiver.openReceiver("127.0.0.1", 0, "", "", 0, error));
    assert(rtpReceiver.openReceiver("127.0.0.1", 0, "", "", 0, error));
    assert(cbrReceiver.openReceiver("127.0.0.1", 0, "", "", 0, error));
    assert(inputSender.openSender("127.0.0.1", inputPort, "", error));

    NativeUdpRelay relay;
    NativeUdpRelayConfig config;
    config.inputUri = "udp://127.0.0.1:" + std::to_string(inputPort);
    config.outputs = {
        {"udp-vbr", "127.0.0.1", udpReceiver.localPort(), ""},
        {"rtp", "127.0.0.1", rtpReceiver.localPort(), ""},
        {"udp-cbr", "127.0.0.1", cbrReceiver.localPort(), ""}
    };
    config.targetBitrate = 512000;
    assert(relay.start(config, error));

    dvbstreamer5::media::mpegts::Packet inputPacket {};
    inputPacket.fill(0xff);
    inputPacket[0] = 0x47;
    inputPacket[1] = 0x01;
    inputPacket[2] = 0x26;
    inputPacket[3] = 0x10;
    assert(inputSender.send(inputPacket.data(), inputPacket.size(), error));

    std::array<std::uint8_t, 2048> received {};
    std::size_t size = 0;
    assert(udpReceiver.receive(received.data(), received.size(), size, 2000, error));
    assert(size == inputPacket.size());
    assert(std::equal(inputPacket.begin(), inputPacket.end(), received.begin()));

    assert(rtpReceiver.receive(received.data(), received.size(), size, 2000, error));
    dvbstreamer5::media::rtp::PacketView view;
    assert(dvbstreamer5::media::rtp::parsePacket(received.data(), size, view));
    std::vector<dvbstreamer5::media::mpegts::Packet> decoded;
    assert(dvbstreamer5::media::rtp::decodeMpegTsPayload(view, decoded));
    assert(decoded.size() == 1 && decoded.front() == inputPacket);

    assert(cbrReceiver.receive(received.data(), received.size(), size, 2000, error));
    assert(size == 7 * 188);
    assert(std::equal(inputPacket.begin(), inputPacket.end(), received.begin()));
    dvbstreamer5::media::mpegts::PacketInfo packetInfo;
    for (std::size_t index = 1; index < 7; ++index) {
        assert(dvbstreamer5::media::mpegts::inspectPacket(
            received.data() + index * 188, 188, packetInfo));
        assert(packetInfo.pid == dvbstreamer5::media::mpegts::kNullPid);
        assert(packetInfo.continuityCounter == index - 1);
    }

    const std::uint64_t expectedOutputBytes = 2 * inputPacket.size() + 7 * 188;
    const auto counterDeadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(1);
    while (relay.outputBytes() < expectedOutputBytes &&
        std::chrono::steady_clock::now() < counterDeadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(relay.outputBytes() == expectedOutputBytes);
    relay.stop();
}

void testNativeFileCbrRelay() {
    using namespace dvbstreamer5::media::network;
    const auto filePath = std::filesystem::temp_directory_path() /
        ("dvbstreamer5-native-cbr-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) + ".ts");
    std::vector<Packet> sourcePackets;
    for (std::uint16_t index = 0; index < 8; ++index) {
        sourcePackets.push_back(packet(
            static_cast<std::uint16_t>(0x0101 + index),
            static_cast<std::uint8_t>((4 + index) & 0x0fU)));
    }
    {
        std::ofstream file(filePath, std::ios::binary);
        assert(file.is_open());
        for (const auto& sourcePacket : sourcePackets) {
            file.write(
                reinterpret_cast<const char*>(sourcePacket.data()),
                static_cast<std::streamsize>(sourcePacket.size()));
        }
        assert(file.good());
    }

    UdpSocket receiver;
    std::string error;
    assert(receiver.openReceiver("127.0.0.1", 0, "", "", 0, error));

    NativeUdpRelay relay;
    NativeUdpRelayConfig config;
    config.inputUri = "file://" + filePath.string();
    config.outputs = {
        {"udp-cbr", "127.0.0.1", receiver.localPort(), ""}
    };
    config.targetBitrate = 512000;
    assert(relay.start(config, error));

    std::array<std::uint8_t, 2048> received {};
    std::size_t size = 0;
    assert(receiver.receive(received.data(), received.size(), size, 2000, error));
    assert(size == 7 * 188);
    for (std::size_t index = 0; index < 7; ++index) {
        assert(std::equal(
            sourcePackets[index].begin(), sourcePackets[index].end(),
            received.begin() + static_cast<std::ptrdiff_t>(index * 188)));
    }
    const auto firstSentAt = std::chrono::steady_clock::now();
    assert(receiver.receive(received.data(), received.size(), size, 1000, error));
    const auto secondSentAt = std::chrono::steady_clock::now();
    assert(size == 7 * 188);
    assert(std::equal(
        sourcePackets[7].begin(), sourcePackets[7].end(), received.begin()));
    assert(secondSentAt - firstSentAt >= std::chrono::milliseconds(10));
    PacketInfo info;
    for (std::size_t index = 1; index < 7; ++index) {
        assert(dvbstreamer5::media::mpegts::inspectPacket(
            received.data() + index * 188, 188, info));
        assert(info.pid == dvbstreamer5::media::mpegts::kNullPid);
        assert(info.continuityCounter == index - 1);
    }

    const auto stopDeadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(1);
    while (relay.isRunning() && std::chrono::steady_clock::now() < stopDeadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(!relay.isRunning());
    assert(relay.lastError().empty());
    assert(relay.inputBytes() == 8 * 188);
    assert(relay.outputBytes() == 2 * 7 * 188);
    assert(receiver.receive(received.data(), received.size(), size, 50, error));
    assert(size == 0);
    relay.stop();
    std::filesystem::remove(filePath);

    {
        std::ofstream file(filePath, std::ios::binary);
        assert(file.is_open());
        const std::array<char, 187> incompletePacket {};
        file.write(incompletePacket.data(), incompletePacket.size());
        assert(file.good());
    }
    config.inputUri = filePath.string();
    assert(relay.start(config, error));
    const auto invalidFileDeadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(1);
    while (relay.isRunning() &&
        std::chrono::steady_clock::now() < invalidFileDeadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(!relay.isRunning());
    assert(relay.lastError() ==
        "native file input contains no complete MPEG-TS packets");
    relay.stop();
    std::filesystem::remove(filePath);
}

void testNativeHttpCbrRelay() {
    using namespace dvbstreamer5::media::network;
    std::vector<std::uint8_t> body;
    std::vector<Packet> sourcePackets;
    for (std::uint16_t index = 0; index < 8; ++index) {
        sourcePackets.push_back(packet(
            static_cast<std::uint16_t>(0x0201 + index),
            static_cast<std::uint8_t>(index & 0x0fU)));
        body.insert(
            body.end(), sourcePackets.back().begin(), sourcePackets.back().end());
    }

    UdpSocket receiver;
    std::string error;
    assert(receiver.openReceiver("127.0.0.1", 0, "", "", 0, error));
    HttpTsTestServer httpServer(std::move(body));

    NativeUdpRelay relay;
    NativeUdpRelayConfig config;
    config.inputUri =
        "http://127.0.0.1:" + std::to_string(httpServer.port()) + "/live.ts";
    config.accessKeyMode = "header";
    config.accessKeyName = "X-Stream-Key";
    config.accessKeyValue = "test-secret";
    config.userAgent = "DVBStreamer5-test";
    config.outputs = {
        {"udp-cbr", "127.0.0.1", receiver.localPort(), ""}
    };
    config.targetBitrate = 512000;
    assert(relay.start(config, error));

    std::array<std::uint8_t, 2048> received {};
    std::size_t size = 0;
    assert(receiver.receive(received.data(), received.size(), size, 3000, error));
    assert(size == 7 * 188);
    for (std::size_t index = 0; index < 7; ++index) {
        assert(std::equal(
            sourcePackets[index].begin(), sourcePackets[index].end(),
            received.begin() + static_cast<std::ptrdiff_t>(index * 188)));
    }

    assert(receiver.receive(received.data(), received.size(), size, 1000, error));
    assert(size == 7 * 188);
    assert(std::equal(
        sourcePackets[7].begin(), sourcePackets[7].end(), received.begin()));
    PacketInfo info;
    for (std::size_t index = 1; index < 7; ++index) {
        assert(dvbstreamer5::media::mpegts::inspectPacket(
            received.data() + index * 188, 188, info));
        assert(info.pid == dvbstreamer5::media::mpegts::kNullPid);
    }

    const auto stopDeadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(2);
    while (relay.isRunning() &&
        std::chrono::steady_clock::now() < stopDeadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(!relay.isRunning());
    relay.stop();
    httpServer.join();
    assert(relay.lastError().empty());
    assert(relay.inputBytes() == 8 * 188);
    assert(relay.outputBytes() == 2 * 7 * 188);
    const std::string request = httpServer.request();
    assert(request.find("X-Stream-Key: test-secret") != std::string::npos);
    assert(request.find("DVBStreamer5-test") != std::string::npos);
}

void testNativeHttpQueryAccessKey() {
    using namespace dvbstreamer5::media::network;
    const Packet sourcePacket = packet(0x0310, 2);
    std::vector<std::uint8_t> body(
        sourcePacket.begin(), sourcePacket.end());
    UdpSocket receiver;
    std::string error;
    assert(receiver.openReceiver("127.0.0.1", 0, "", "", 0, error));
    HttpTsTestServer httpServer(std::move(body));

    NativeUdpRelay relay;
    NativeUdpRelayConfig config;
    config.inputUri =
        "http://127.0.0.1:" + std::to_string(httpServer.port()) +
        "/live.ts#client-fragment";
    config.accessKeyMode = "query";
    config.accessKeyName = "stream key";
    config.accessKeyValue = "a&b";
    config.outputs = {
        {"udp-cbr", "127.0.0.1", receiver.localPort(), ""}
    };
    config.targetBitrate = 512000;
    assert(relay.start(config, error));

    std::array<std::uint8_t, 2048> received {};
    std::size_t size = 0;
    assert(receiver.receive(received.data(), received.size(), size, 3000, error));
    assert(size == 7 * 188);
    assert(std::equal(
        sourcePacket.begin(), sourcePacket.end(), received.begin()));

    const auto stopDeadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(2);
    while (relay.isRunning() &&
        std::chrono::steady_clock::now() < stopDeadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(!relay.isRunning());
    relay.stop();
    httpServer.join();
    assert(relay.lastError().empty());
    const std::string request = httpServer.request();
    assert(request.find("GET /live.ts?stream%20key=a%26b HTTP/1.1") !=
        std::string::npos);
}

void testNativeHttpVbrAndRtpFanout() {
    using namespace dvbstreamer5::media::network;
    const Packet sourcePacket = packet(0x0321, 8);
    std::vector<std::uint8_t> body(sourcePacket.begin(), sourcePacket.end());
    UdpSocket udpReceiver;
    UdpSocket rtpReceiver;
    std::string error;
    assert(udpReceiver.openReceiver("127.0.0.1", 0, "", "", 0, error));
    assert(rtpReceiver.openReceiver("127.0.0.1", 0, "", "", 0, error));
    HttpTsTestServer httpServer(std::move(body));

    NativeUdpRelay relay;
    NativeUdpRelayConfig config;
    config.inputUri =
        "http://127.0.0.1:" + std::to_string(httpServer.port()) + "/fanout.ts";
    config.outputs = {
        {"udp-vbr", "127.0.0.1", udpReceiver.localPort(), ""},
        {"rtp", "127.0.0.1", rtpReceiver.localPort(), ""}
    };
    assert(relay.start(config, error));

    std::array<std::uint8_t, 2048> received {};
    std::size_t size = 0;
    assert(udpReceiver.receive(received.data(), received.size(), size, 3000, error));
    assert(size == sourcePacket.size());
    assert(std::equal(sourcePacket.begin(), sourcePacket.end(), received.begin()));

    assert(rtpReceiver.receive(received.data(), received.size(), size, 1000, error));
    dvbstreamer5::media::rtp::PacketView view;
    assert(dvbstreamer5::media::rtp::parsePacket(received.data(), size, view));
    std::vector<Packet> decoded;
    assert(dvbstreamer5::media::rtp::decodeMpegTsPayload(view, decoded));
    assert(decoded.size() == 1 && decoded.front() == sourcePacket);

    const auto stopDeadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(2);
    while (relay.isRunning() &&
        std::chrono::steady_clock::now() < stopDeadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(!relay.isRunning());
    relay.stop();
    httpServer.join();
    assert(relay.lastError().empty());
    assert(relay.inputBytes() == sourcePacket.size());
    assert(relay.outputBytes() == 2 * sourcePacket.size());
}

void testNativeHttpFailureIsReported() {
    using namespace dvbstreamer5::media::network;
    UdpSocket receiver;
    std::string error;
    assert(receiver.openReceiver("127.0.0.1", 0, "", "", 0, error));
    HttpTsTestServer httpServer({}, 404);

    NativeUdpRelay relay;
    NativeUdpRelayConfig config;
    config.inputUri =
        "http://127.0.0.1:" + std::to_string(httpServer.port()) + "/missing.ts";
    config.outputs = {
        {"udp-cbr", "127.0.0.1", receiver.localPort(), ""}
    };
    assert(relay.start(config, error));

    const auto stopDeadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(2);
    while (relay.isRunning() &&
        std::chrono::steady_clock::now() < stopDeadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(!relay.isRunning());
    relay.stop();
    httpServer.join();
    assert(relay.lastError().find("HTTP 404") != std::string::npos);
}

void testNativeHttpDoesNotRedirectAccessKeys() {
    using namespace dvbstreamer5::media::network;
    UdpSocket receiver;
    std::string error;
    assert(receiver.openReceiver("127.0.0.1", 0, "", "", 0, error));
    HttpTsTestServer httpServer(
        {}, 302, "http://127.0.0.1:1/redirected.ts");

    NativeUdpRelay relay;
    NativeUdpRelayConfig config;
    config.inputUri =
        "http://127.0.0.1:" + std::to_string(httpServer.port()) + "/secure.ts";
    config.accessKeyMode = "header";
    config.accessKeyName = "X-Stream-Key";
    config.accessKeyValue = "must-not-leak";
    config.outputs = {
        {"udp-cbr", "127.0.0.1", receiver.localPort(), ""}
    };
    assert(relay.start(config, error));

    const auto stopDeadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(2);
    while (relay.isRunning() &&
        std::chrono::steady_clock::now() < stopDeadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(!relay.isRunning());
    relay.stop();
    httpServer.join();
    assert(relay.lastError().find("HTTP 302") != std::string::npos);
    const std::string request = httpServer.request();
    assert(request.find("X-Stream-Key: must-not-leak") != std::string::npos);
}

void testNativePreviewHubFanout() {
#if !defined(_WIN32)
    using dvbstreamer5::media::network::NativePreviewHub;
    NativePreviewHub hub;
    std::string error;
    const int first = hub.subscribe(error);
    const int second = hub.subscribe(error);
    assert(first >= 0 && second >= 0);

    const Packet source = packet(0x0123, 4);
    hub.publish(source.data(), source.size());
    std::array<std::uint8_t, 2048> received {};
    assert(::read(first, received.data(), received.size()) ==
        static_cast<ssize_t>(source.size()));
    assert(std::equal(source.begin(), source.end(), received.begin()));
    assert(::read(second, received.data(), received.size()) ==
        static_cast<ssize_t>(source.size()));
    assert(std::equal(source.begin(), source.end(), received.begin()));

    hub.unsubscribe(first);
    ::close(first);
    hub.close();
    assert(::read(second, received.data(), received.size()) == 0);
    ::close(second);
#endif
}

} // namespace

int main() {
    testLinuxDvbPidListParsing();
    testNativeMpegTsMux();
    testMpegTsRemapper();
    testPacketInspectionAndPidRewrite();
    testFraming();
    testContinuityTracking();
    testRtpParsingAndPayload();
    testRtpPacketizerRoundTrip();
    testUdpLoopback();
    testNativeUdpTsRelay();
    testNativeRtpTsRelay();
    testNativeExternallyFedTsRelay();
    testCbrTsPacer();
    testNativeUdpCbrRelay();
    testNativeUdpFanoutRelay();
    testNativeFileCbrRelay();
    testNativeHttpCbrRelay();
    testNativeHttpQueryAccessKey();
    testNativeHttpVbrAndRtpFanout();
    testNativeHttpFailureIsReported();
    testNativeHttpDoesNotRedirectAccessKeys();
    testNativePreviewHubFanout();
    std::cout << "PASS: native MPEG-TS/RTP and CBR output relay over UDP\n";
}
