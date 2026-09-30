#pragma once

#include "ConfigManager.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace dvbstreamer5::media::hls {

class NativeHlsInput {
public:
    using DataCallback = std::function<bool(const std::uint8_t*, std::size_t)>;
    using FinishCallback = std::function<void(const std::string&)>;

    NativeHlsInput() = default;
    ~NativeHlsInput();

    NativeHlsInput(const NativeHlsInput&) = delete;
    NativeHlsInput& operator=(const NativeHlsInput&) = delete;

    bool start(const StreamConfig& config,
               DataCallback dataCallback,
               FinishCallback finishCallback,
               std::string& error);
    void stop() noexcept;

    bool isRunning() const noexcept;
    std::uint64_t inputBytes() const noexcept;
    std::uint64_t mediaBitrate() const noexcept;
    std::string lastError() const;

private:
    void run();
    void setError(const std::string& error);

    StreamConfig config_;
    DataCallback dataCallback_;
    FinishCallback finishCallback_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> inputBytes_{0};
    std::atomic<std::uint64_t> mediaBitrate_{0};
    mutable std::mutex errorMutex_;
    std::string lastError_;
    std::thread worker_;
};

} // namespace dvbstreamer5::media::hls
