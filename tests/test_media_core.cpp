#include "media/RtpMpegTs.h"
#include "media/NativeUdpRelay.h"
#include "media/CbrTsPacer.h"
#include "media/UdpSocket.h"

#ifdef NDEBUG
#undef NDEBUG
#endif

#include <algorithm>
#include <array>
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

using tvs::media::mpegts::ContinuityStatus;
using tvs::media::mpegts::Packet;
using tvs::media::mpegts::PacketInfo;

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
    assert(tvs::media::mpegts::inspectPacket(ts.data(), ts.size(), info));
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
    assert(tvs::media::mpegts::inspectPacket(ts.data(), ts.size(), info));
    assert(info.hasAdaptationField && info.hasPcr && info.discontinuity);
    assert(info.payloadOffset == 12);
    assert(info.pcrBase90k == 33818120);
    assert(tvs::media::mpegts::rewritePid(ts.data(), ts.size(), 0x1ffe));
    assert(tvs::media::mpegts::inspectPacket(ts.data(), ts.size(), info));
    assert(info.pid == 0x1ffe && info.hasPcr);

    const Packet before = ts;
    assert(!tvs::media::mpegts::rewritePid(ts.data(), ts.size(), 0x2000));
    assert(ts == before);
    ts[0] = 0;
    assert(!tvs::media::mpegts::inspectPacket(ts.data(), ts.size(), info));
    ts = packet(1, 0);
    ts[3] &= 0x0f;
    assert(!tvs::media::mpegts::inspectPacket(ts.data(), ts.size(), info));
    ts = packet(1, 0);
    ts[3] = 0x30;
    ts[4] = 184;
    assert(!tvs::media::mpegts::inspectPacket(ts.data(), ts.size(), info));
    ts[4] = 1;
    ts[5] = 0x10;
    assert(!tvs::media::mpegts::inspectPacket(ts.data(), ts.size(), info));
}

void testFraming() {
    using tvs::media::mpegts::PacketFramer;
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
    using tvs::media::mpegts::ContinuityTracker;
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
    using tvs::media::rtp::PacketView;
    using tvs::media::rtp::decodeMpegTsPayload;
    using tvs::media::rtp::parsePacket;

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
    using tvs::media::rtp::MpegTsPacketizer;
    using tvs::media::rtp::PacketView;
    using tvs::media::rtp::decodeMpegTsPayload;
    using tvs::media::rtp::parsePacket;

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
    using tvs::media::network::UdpSocket;
    UdpSocket receiver;
    UdpSocket sender;
    std::string error;
    assert(receiver.openReceiver("127.0.0.1", 0, "", "", 0, error));
    assert(receiver.localPort() != 0);
    assert(sender.openSender("127.0.0.1", receiver.localPort(), "127.0.0.1", error));

    const Packet expected = packet(0x0100, 4);
    tvs::media::rtp::MpegTsPacketizer packetizer(0x55667788);
    std::vector<std::vector<std::uint8_t>> datagrams;
    assert(packetizer.packetize({expected}, 90000, datagrams));
    assert(sender.send(datagrams.front().data(), datagrams.front().size(), error));

    std::array<std::uint8_t, 2048> buffer {};
    std::size_t received = 0;
    assert(receiver.receive(buffer.data(), buffer.size(), received, 2000, error));
    assert(received == datagrams.front().size());
    tvs::media::rtp::PacketView rtp;
    assert(tvs::media::rtp::parsePacket(buffer.data(), received, rtp));
    assert(rtp.timestamp == 90000 && rtp.sourceId == 0x55667788);
    std::vector<Packet> decoded;
    assert(tvs::media::rtp::decodeMpegTsPayload(rtp, decoded));
    assert(decoded.size() == 1 && decoded.front() == expected);

    received = 99;
    assert(receiver.receive(buffer.data(), buffer.size(), received, 5, error));
    assert(received == 0);
    assert(!sender.send(nullptr, 0, error));
}

std::uint16_t reserveLocalUdpPort() {
    tvs::media::network::UdpSocket socket;
    std::string error;
    assert(socket.openReceiver("127.0.0.1", 0, "", "", 0, error));
    const std::uint16_t port = socket.localPort();
    socket.close();
    return port;
}

void testNativeUdpTsRelay() {
    using namespace tvs::media::network;
    const std::uint16_t inputPort = reserveLocalUdpPort();

    UdpSocket outputReceiver;
    UdpSocket inputSender;
    std::string error;
    assert(outputReceiver.openReceiver("127.0.0.1", 0, "", "", 0, error));
    assert(inputSender.openSender("127.0.0.1", inputPort, "", error));

    NativeUdpRelay relay;
    NativeUdpRelayConfig config;
    config.inputUri = "udp://127.0.0.1:" + std::to_string(inputPort);
    config.outputType = "udp-vbr";
    config.outputHost = "127.0.0.1";
    config.outputPort = outputReceiver.localPort();
    assert(relay.start(config, error));
    assert(relay.isRunning());

    const Packet expected = packet(0x0137, 8);
    assert(inputSender.send(expected.data(), expected.size(), error));

    std::array<std::uint8_t, 2048> received {};
    std::size_t size = 0;
    assert(outputReceiver.receive(received.data(), received.size(), size, 2000, error));
    assert(size == expected.size());
    assert(std::equal(expected.begin(), expected.end(), received.begin()));
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
    using namespace tvs::media::network;
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
    tvs::media::rtp::MpegTsPacketizer packetizer(0x76543210, 14);
    std::vector<std::vector<std::uint8_t>> sourceDatagrams;
    assert(packetizer.packetize({expected}, 90000, sourceDatagrams));
    assert(inputSender.send(
        sourceDatagrams.front().data(), sourceDatagrams.front().size(), error));

    std::array<std::uint8_t, 2048> received {};
    std::size_t size = 0;
    assert(outputReceiver.receive(received.data(), received.size(), size, 2000, error));
    tvs::media::rtp::PacketView view;
    assert(tvs::media::rtp::parsePacket(received.data(), size, view));
    assert(view.payloadType == 33 && view.payloadSize == expected.size());
    std::vector<Packet> decoded;
    assert(tvs::media::rtp::decodeMpegTsPayload(view, decoded));
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

void testCbrTsPacer() {
    using namespace tvs::media::mpegts;
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
    using namespace tvs::media::network;
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
    assert(tvs::media::mpegts::inspectPacket(received.data(), 188, info));
    assert(info.pid == 0x0175);
    for (std::size_t i = 1; i < 7; ++i) {
        assert(tvs::media::mpegts::inspectPacket(received.data() + i * 188, 188, info));
        assert(info.pid == tvs::media::mpegts::kNullPid);
        assert(info.continuityCounter == i - 1);
    }

    const auto firstSentAt = std::chrono::steady_clock::now();
    assert(outputReceiver.receive(received.data(), received.size(), size, 1000, error));
    const auto secondSentAt = std::chrono::steady_clock::now();
    assert(size == 7 * 188);
    assert(secondSentAt - firstSentAt >= std::chrono::milliseconds(15));
    for (std::size_t i = 0; i < 7; ++i) {
        assert(tvs::media::mpegts::inspectPacket(received.data() + i * 188, 188, info));
        assert(info.pid == tvs::media::mpegts::kNullPid);
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
    using namespace tvs::media::network;
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

    tvs::media::mpegts::Packet inputPacket {};
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
    tvs::media::rtp::PacketView view;
    assert(tvs::media::rtp::parsePacket(received.data(), size, view));
    std::vector<tvs::media::mpegts::Packet> decoded;
    assert(tvs::media::rtp::decodeMpegTsPayload(view, decoded));
    assert(decoded.size() == 1 && decoded.front() == inputPacket);

    assert(cbrReceiver.receive(received.data(), received.size(), size, 2000, error));
    assert(size == 7 * 188);
    assert(std::equal(inputPacket.begin(), inputPacket.end(), received.begin()));
    tvs::media::mpegts::PacketInfo packetInfo;
    for (std::size_t index = 1; index < 7; ++index) {
        assert(tvs::media::mpegts::inspectPacket(
            received.data() + index * 188, 188, packetInfo));
        assert(packetInfo.pid == tvs::media::mpegts::kNullPid);
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
    using namespace tvs::media::network;
    const auto filePath = std::filesystem::temp_directory_path() /
        ("tvs-native-cbr-" + std::to_string(
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
        assert(tvs::media::mpegts::inspectPacket(
            received.data() + index * 188, 188, info));
        assert(info.pid == tvs::media::mpegts::kNullPid);
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
    using namespace tvs::media::network;
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
        assert(tvs::media::mpegts::inspectPacket(
            received.data() + index * 188, 188, info));
        assert(info.pid == tvs::media::mpegts::kNullPid);
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
    using namespace tvs::media::network;
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
    using namespace tvs::media::network;
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
    tvs::media::rtp::PacketView view;
    assert(tvs::media::rtp::parsePacket(received.data(), size, view));
    std::vector<Packet> decoded;
    assert(tvs::media::rtp::decodeMpegTsPayload(view, decoded));
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
    using namespace tvs::media::network;
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
    using namespace tvs::media::network;
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

} // namespace

int main() {
    testPacketInspectionAndPidRewrite();
    testFraming();
    testContinuityTracking();
    testRtpParsingAndPayload();
    testRtpPacketizerRoundTrip();
    testUdpLoopback();
    testNativeUdpTsRelay();
    testNativeRtpTsRelay();
    testCbrTsPacer();
    testNativeUdpCbrRelay();
    testNativeUdpFanoutRelay();
    testNativeFileCbrRelay();
    testNativeHttpCbrRelay();
    testNativeHttpQueryAccessKey();
    testNativeHttpVbrAndRtpFanout();
    testNativeHttpFailureIsReported();
    testNativeHttpDoesNotRedirectAccessKeys();
    std::cout << "PASS: native MPEG-TS/RTP and CBR output relay over UDP\n";
}
