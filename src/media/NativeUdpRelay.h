#pragma once

#include "media/CbrTsPacer.h"
#include "media/LinuxDvbInput.h"
#include "media/MpegTsRemapper.h"
#include "media/UdpSocket.h"

#include <curl/curl.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tvs::media::network {

struct NativeUdpRelayOutputConfig {
    std::string outputType;
    std::string outputHost;
    int outputPort = 0;
    std::string interfaceAddress;
};

struct NativeUdpRelayConfig {
    std::string inputUri;
    bool dvbInputSource = false;
    LinuxDvbTuneConfig dvbTuneConfig;
    bool remapEnabled = false;
    mpegts::RemapConfig remapConfig;
    std::string inputInterfaceAddress;
    std::string inputInterfaceDeviceName;
    bool inputInterfaceAddressConfigured = false;
    std::string accessKeyMode;
    std::string accessKeyName;
    std::string accessKeyValue;
    std::string userAgent;
    std::string interfaceAddress;
    std::string outputType;
    std::string outputHost;
    int outputPort = 0;
    std::uint64_t targetBitrate = 2000000;
    std::vector<NativeUdpRelayOutputConfig> outputs;
};

class NativeUdpRelay {
public:
    NativeUdpRelay() = default;
    ~NativeUdpRelay();

    NativeUdpRelay(const NativeUdpRelay&) = delete;
    NativeUdpRelay& operator=(const NativeUdpRelay&) = delete;

    bool start(const NativeUdpRelayConfig& config, std::string& error);
    void stop() noexcept;

    bool isRunning() const noexcept;
    std::uint64_t inputBytes() const noexcept;
    std::uint64_t outputBytes() const noexcept;
    std::uint64_t continuityErrors() const noexcept;
    std::string lastError() const;

private:
    void run();
    void runHttpInput();
    bool enqueueHttpData(const std::uint8_t* data, std::size_t size);
    void finishHttpInput(const std::string& error);
    static std::size_t curlWrite(
        char* data, std::size_t size, std::size_t count, void* userData);
    static int curlProgress(
        void* userData, curl_off_t, curl_off_t, curl_off_t, curl_off_t);

    NativeUdpRelayConfig config_;
    UdpSocket inputSocket_;
    std::vector<std::unique_ptr<UdpSocket>> outputSockets_;
    std::ifstream fileInput_;
    bool fileInputSource_ = false;
    bool httpInputSource_ = false;
    bool dvbInputSource_ = false;
    LinuxDvbInput dvbInput_;
    mpegts::Remapper remapper_;
    std::thread httpWorker_;
    std::mutex httpQueueMutex_;
    std::condition_variable httpQueueCondition_;
    std::deque<std::vector<std::uint8_t>> httpQueue_;
    std::size_t httpQueuedBytes_ = 0;
    bool httpFinished_ = false;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> inputBytes_{0};
    std::atomic<std::uint64_t> outputBytes_{0};
    std::atomic<std::uint64_t> continuityErrors_{0};
    mutable std::mutex errorMutex_;
    std::string lastError_;
    std::thread worker_;
};

} // namespace tvs::media::network
