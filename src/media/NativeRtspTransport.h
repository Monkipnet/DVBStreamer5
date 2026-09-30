#pragma once

#include "media/NativeMpegTsMux.h"
#include "media/RtpMpegTs.h"
#include "media/TransportStream.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dvbstreamer5::media::rtsp {

struct InputConfig {
    std::string uri;
    std::string mode = "auto"; // auto | tcp | udp
    std::string bindAddress;
    int connectTimeoutMs = 5000;
    int ioTimeoutMs = 5000;
};

class NativeRtspInput {
public:
    using DataCallback = std::function<bool(const std::uint8_t*, std::size_t)>;
    using StatusCallback = std::function<void(const std::string&)>;

    NativeRtspInput() = default;
    ~NativeRtspInput();
    NativeRtspInput(const NativeRtspInput&) = delete;
    NativeRtspInput& operator=(const NativeRtspInput&) = delete;

    bool start(const InputConfig& config, DataCallback data, StatusCallback status, std::string& error);
    void stop() noexcept;
    bool isRunning() const noexcept { return running_.load(); }
    std::uint64_t receivedBytes() const noexcept { return receivedBytes_.load(); }
    std::string lastError() const;

private:
    void run();
    void setError(const std::string& value);

    InputConfig config_;
    DataCallback data_;
    StatusCallback status_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> receivedBytes_{0};
    mutable std::mutex errorMutex_;
    std::string lastError_;
    std::thread worker_;
};

struct OutputConfig {
    std::string bindAddress = "0.0.0.0";
    int port = 8554;
    std::string streamName = "stream";
};

class NativeRtspOutput {
public:
    using AllowPeer = std::function<bool(const std::string&)>;
    using PeerCallback = std::function<void(const std::string&)>;

    NativeRtspOutput();
    ~NativeRtspOutput();
    NativeRtspOutput(const NativeRtspOutput&) = delete;
    NativeRtspOutput& operator=(const NativeRtspOutput&) = delete;

    bool start(const OutputConfig& config, AllowPeer allowPeer,
               PeerCallback onConnect, PeerCallback onDisconnect, std::string& error);
    void stop() noexcept;
    bool push(const std::uint8_t* data, std::size_t size);
    bool isRunning() const noexcept { return running_.load(); }
    std::uint64_t sentBytes() const noexcept { return sentBytes_.load(); }
    std::string lastError() const;

private:
    struct Client;
    void acceptLoop();
    void clientLoop(const std::shared_ptr<Client>& client);
    void removeClient(const std::shared_ptr<Client>& client);
    void setError(const std::string& value);

    OutputConfig config_;
    AllowPeer allowPeer_;
    PeerCallback onConnect_;
    PeerCallback onDisconnect_;
    int listenFd_ = -1;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> sentBytes_{0};
    std::thread acceptThread_;
    mutable std::mutex clientsMutex_;
    std::vector<std::shared_ptr<Client>> clients_;
    mpegts::PacketFramer framer_;
    rtp::MpegTsPacketizer packetizer_;
    mutable std::mutex packetizerMutex_;
    mutable std::mutex errorMutex_;
    std::string lastError_;
};

} // namespace dvbstreamer5::media::rtsp
