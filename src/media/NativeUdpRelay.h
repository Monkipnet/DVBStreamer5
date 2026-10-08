#pragma once

#include "media/CbrTsPacer.h"
#include "media/LinuxDvbInput.h"
#include "media/MpegTsRemapper.h"
#include "media/NativePreviewHub.h"
#include "media/UdpSocket.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dvbstreamer5::media::network {

struct NativeUdpRelayOutputConfig {
    std::string outputType;
    std::string outputHost;
    int outputPort = 0;
    std::string interfaceAddress;
};

// Thread-safe transport observer with a separate fan-out used by the public
// HTTP MPEG-TS output.  The primary callback keeps the existing HLS/SRT/RTSP/
// RTMP/MPTS behavior, while HTTP subscribers receive the already processed
// post-remap/post-CA/post-transcode transport directly.  This deliberately does
// not share NativePreviewHub subscribers with the browser preview transcoder.
class NativeTransportObserver {
public:
    using Callback = std::function<void(const std::uint8_t*, std::size_t)>;

    NativeTransportObserver()
        : httpHub_(std::make_shared<NativePreviewHub>()) {}

    NativeTransportObserver(const NativeTransportObserver&) = default;
    NativeTransportObserver& operator=(const NativeTransportObserver&) = default;

    NativeTransportObserver& operator=(Callback callback) {
        callback_ = std::move(callback);
        return *this;
    }

    explicit operator bool() const {
        return static_cast<bool>(callback_) ||
            (httpHub_ && httpHub_->subscriberCount() != 0U);
    }

    void operator()(const std::uint8_t* data, std::size_t size) const {
        if (callback_) callback_(data, size);
        if (httpHub_) httpHub_->publish(data, size);
    }

    std::shared_ptr<NativePreviewHub> httpHub() const {
        return httpHub_;
    }

private:
    // V10.8.136: NativeUdpRelayConfig is fully assembled before start(), then
    // copied into NativeUdpRelay before the worker thread is launched.  The
    // observer callback and hub pointer are immutable afterwards, so the hot
    // 1316-byte transport path needs neither a mutex nor an atomic shared_ptr
    // snapshot/refcount operation. NativePreviewHub remains independently
    // thread-safe for subscriber attach/detach and publish.
    Callback callback_;
    std::shared_ptr<NativePreviewHub> httpHub_;
};

struct NativeUdpRelayConfig {
    std::string inputUri;
    bool externallyFedInput = false;
    // V10.8.137: set only when an external producer already emits contiguous,
    // structurally validated 188-byte MPEG-TS packets (shared-DVB prefilter).
    // Other external inputs retain the legacy full PacketFramer validation.
    bool trustedAlignedExternalInput = false;
    bool dvbInputSource = false;
    LinuxDvbTuneConfig dvbTuneConfig;
    bool remapEnabled = false;
    mpegts::RemapConfig remapConfig;
    std::function<bool(std::uint8_t*, std::size_t)> processTransport;
    // Optional one-to-many transport transform. Used by the native transcoder:
    // input is a contiguous MPEG-TS block after remap/CA, output is a new
    // contiguous MPEG-TS block. Empty output is valid while codecs buffer.
    std::function<bool(const std::uint8_t*, std::size_t, std::vector<std::uint8_t>&, std::string&)> transformTransport;
    // Optional tap of the post-remap/post-CA input before the primary transform.
    // Native HLS ABR uses this to feed additional independent rendition encoders
    // from the exact same clean MPEG-TS input without re-reading the source.
    std::function<void(const std::uint8_t*, std::size_t)> observeInputTransport;
    NativeTransportObserver observeTransport;
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
    // Pace observeTransport consumers (SRT/RTSP/RTMP/HLS/HTTP) at the
    // configured CBR instead of delivering mux bursts immediately.
    bool paceObservedTransport = false;
    std::vector<NativeUdpRelayOutputConfig> outputs;
    bool allowNoNetworkOutput = false;
};

class NativeUdpRelay {
public:
    NativeUdpRelay() = default;
    ~NativeUdpRelay();

    NativeUdpRelay(const NativeUdpRelay&) = delete;
    NativeUdpRelay& operator=(const NativeUdpRelay&) = delete;

    bool start(const NativeUdpRelayConfig& config, std::string& error);
    bool pushInput(const std::uint8_t* data, std::size_t size);
    void finishInput(const std::string& error = {});
    void stop() noexcept;

    bool isRunning() const noexcept;
    std::uint64_t inputBytes() const noexcept;
    std::uint64_t sourceInputBytes() const noexcept;
    // Non-null MPEG-TS bytes after service remap/CA and before transcoding.
    // For shared DVB this is the selected channel bitrate, not the whole mux.
    std::uint64_t selectedInputBytes() const noexcept;
    std::uint64_t outputBytes() const noexcept;
    std::uint64_t payloadOutputBytes() const noexcept;
    std::uint64_t continuityErrors() const noexcept;
    std::string lastError() const;

    std::shared_ptr<NativePreviewHub> httpOutputHub() const {
        return config_.observeTransport.httpHub();
    }

private:
    void run();
    void runHttpInput();
    bool enqueueHttpData(const std::uint8_t* data, std::size_t size);
    void finishHttpInput(const std::string& error);

    NativeUdpRelayConfig config_;
    UdpSocket inputSocket_;
    std::vector<std::unique_ptr<UdpSocket>> outputSockets_;
    std::ifstream fileInput_;
    bool fileInputSource_ = false;
    bool httpInputSource_ = false;
    bool externalInputSource_ = false;
    bool dvbInputSource_ = false;
    LinuxDvbInput dvbInput_;
    mpegts::Remapper remapper_;
    std::thread httpWorker_;
    std::mutex httpQueueMutex_;
    std::condition_variable httpQueueCondition_;
    std::deque<std::vector<std::uint8_t>> httpQueue_;
    // V10.8.138: recycle a small number of consumed queue buffers so the
    // shared-DVB/external hot path does not malloc/free one vector per chunk.
    std::vector<std::vector<std::uint8_t>> httpQueueBufferPool_;
    std::size_t httpQueuedBytes_ = 0;
    // V10.8.139: guarded by httpQueueMutex_. Only signal queue-space
    // availability when the single producer is actually blocked.
    bool httpQueueProducerWaiting_ = false;
    bool httpFinished_ = false;
    std::atomic<bool> httpStopRequested_{false};
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> inputBytes_{0};
    std::atomic<std::uint64_t> httpReceivedBytes_{0};
    std::atomic<std::uint64_t> selectedInputBytes_{0};
    std::atomic<std::uint64_t> outputBytes_{0};
    std::atomic<std::uint64_t> payloadOutputBytes_{0};
    std::atomic<std::uint64_t> continuityErrors_{0};
    mutable std::mutex errorMutex_;
    std::string lastError_;
    std::thread worker_;
};

} // namespace dvbstreamer5::media::network
