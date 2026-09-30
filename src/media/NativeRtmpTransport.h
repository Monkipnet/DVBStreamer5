#pragma once

#include "media/NativeMpegTsMux.h"
#include "media/NativeTsDemux.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dvbstreamer5::media::rtmp {

struct EndpointConfig {
    std::string uri;
    std::string bindAddress;
    int connectTimeoutMs = 5000;
    int ioTimeoutMs = 5000;
};

class NativeRtmpInput {
public:
    using DataCallback = std::function<bool(const std::uint8_t*, std::size_t)>;
    using StatusCallback = std::function<void(const std::string&)>;
    NativeRtmpInput() = default;
    ~NativeRtmpInput();
    bool start(const EndpointConfig& config, DataCallback data, StatusCallback status, std::string& error);
    void stop() noexcept;
    bool isRunning() const noexcept { return running_.load(); }
    std::uint64_t receivedBytes() const noexcept { return receivedBytes_.load(); }
    std::string lastError() const;
private:
    void run();
    void setError(const std::string& value);
    EndpointConfig config_;
    DataCallback data_;
    StatusCallback status_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> receivedBytes_{0};
    mutable std::mutex errorMutex_;
    std::string lastError_;
    std::thread worker_;
};

class NativeRtmpOutput {
public:
    using StatusCallback = std::function<void(const std::string&)>;
    NativeRtmpOutput();
    ~NativeRtmpOutput();
    bool start(const EndpointConfig& config, StatusCallback status, std::string& error);
    void stop() noexcept;
    bool push(const std::uint8_t* data, std::size_t size);
    bool isRunning() const noexcept { return running_.load(); }
    std::uint64_t sentBytes() const noexcept { return sentBytes_.load(); }
    std::string lastError() const;
private:
    struct Impl;
    void setError(const std::string& value);
    EndpointConfig config_;
    StatusCallback status_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> sentBytes_{0};
    mutable std::mutex errorMutex_;
    std::string lastError_;
    std::unique_ptr<Impl> impl_;
};

} // namespace dvbstreamer5::media::rtmp
