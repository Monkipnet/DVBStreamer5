#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dvbstreamer5::media::srt {

struct EndpointConfig {
    std::string host;
    int port = 0;
    std::string mode = "caller"; // caller | listener
    std::string bindAddress;
    int latencyMs = 120;
    int receiveLatencyMs = 0;
    int peerLatencyMs = 0;
    int connectTimeoutMs = 3000;
    int ioTimeoutMs = 1000;
    int payloadSize = 1316;
    int receiveBufferBytes = 0;
    int sendBufferBytes = 0;
    int flightWindowPackets = 0;
    std::string passphrase;
    int pbkeylen = 0;
    std::string streamId;
};

bool parseUri(const std::string& uri, EndpointConfig& config, std::string& error);
std::string buildUri(const EndpointConfig& config, bool includeSecrets = false);
bool runtimeAvailable(std::string* detail = nullptr);
std::uint32_t runtimeVersion();

class NativeSrtInput {
public:
    using DataCallback = std::function<bool(const std::uint8_t*, std::size_t)>;
    using StateCallback = std::function<void(const std::string&)>;
    ~NativeSrtInput();
    NativeSrtInput() = default;
    NativeSrtInput(const NativeSrtInput&) = delete;
    NativeSrtInput& operator=(const NativeSrtInput&) = delete;

    bool start(const EndpointConfig& config, DataCallback onData, StateCallback onState, std::string& error);
    void stop() noexcept;
    bool running() const noexcept { return running_.load(std::memory_order_acquire); }
    std::uint64_t receivedBytes() const noexcept { return receivedBytes_.load(std::memory_order_relaxed); }
    std::uint64_t reconnects() const noexcept { return reconnects_.load(std::memory_order_relaxed); }
    std::string lastError() const;

private:
    void run();
    void setError(const std::string& value);
    EndpointConfig config_;
    DataCallback onData_;
    StateCallback onState_;
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> receivedBytes_{0};
    std::atomic<std::uint64_t> reconnects_{0};
    mutable std::mutex errorMutex_;
    std::string lastError_;
    std::thread worker_;
};

class NativeSrtOutput {
public:
    using PeerAllowedCallback = std::function<bool(const std::string&)>;
    using PeerStateCallback = std::function<void(const std::string&)>;
    ~NativeSrtOutput();
    NativeSrtOutput() = default;
    NativeSrtOutput(const NativeSrtOutput&) = delete;
    NativeSrtOutput& operator=(const NativeSrtOutput&) = delete;

    bool start(const EndpointConfig& config, PeerAllowedCallback peerAllowed,
               PeerStateCallback onConnected, PeerStateCallback onDisconnected,
               std::string& error);
    bool push(const std::uint8_t* data, std::size_t size);
    void stop() noexcept;
    bool running() const noexcept { return running_.load(std::memory_order_acquire); }
    bool connected() const noexcept { return connected_.load(std::memory_order_acquire); }
    std::uint64_t sentBytes() const noexcept { return sentBytes_.load(std::memory_order_relaxed); }
    std::uint64_t droppedBytes() const noexcept { return droppedBytes_.load(std::memory_order_relaxed); }
    std::uint64_t reconnects() const noexcept { return reconnects_.load(std::memory_order_relaxed); }
    std::string lastError() const;

private:
    void run();
    void setError(const std::string& value);
    bool popChunk(std::vector<std::uint8_t>& chunk);
    void clearQueuedData();
    EndpointConfig config_;
    PeerAllowedCallback peerAllowed_;
    PeerStateCallback onConnected_;
    PeerStateCallback onDisconnected_;
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::atomic<std::uint64_t> sentBytes_{0};
    std::atomic<std::uint64_t> droppedBytes_{0};
    std::atomic<std::uint64_t> reconnects_{0};
    mutable std::mutex errorMutex_;
    std::string lastError_;
    std::mutex queueMutex_;
    std::condition_variable queueCondition_;
    std::deque<std::vector<std::uint8_t>> queue_;
    std::size_t queuedBytes_ = 0;
    std::thread worker_;
};

} // namespace dvbstreamer5::media::srt
