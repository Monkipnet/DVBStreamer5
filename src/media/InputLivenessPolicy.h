#pragma once
// V519: distinguish real input loss from bursty network ingress.
// Selected non-NULL TS progress is the only recovery proof. Network socket
// bytes grant a *bounded* parse/backlog grace, never indefinite "healthy".
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>

namespace dvbstreamer5::media::network {

class InputLivenessPolicy {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using Milliseconds = std::chrono::milliseconds;

    struct Sample {
        bool mediaAdvanced = false;
        bool sourceAdvanced = false;
        bool stalled = false;
        bool recovered = false;
        bool jitterGrace = false;
        std::uint64_t mediaSilenceMs = 0;
        std::uint64_t sourceSilenceMs = 0;
        const char* cause = "healthy";
    };

    static bool isNetworkSource(const std::string& uri,
                                const std::string& mode) noexcept {
        const auto starts = [&](const char* protocol) {
            return uri.rfind(protocol, 0) == 0;
        };
        // Uri and input mode are already lower-case in the caller.
        return starts("udp://") || starts("rtp://") ||
               starts("http://") || starts("https://") ||
               starts("srt://") || starts("rtsp://") ||
               starts("rtmp://") || starts("rtmps://") ||
               starts("hls://") || uri.find(".m3u8") != std::string::npos ||
               mode == "udp" || mode == "rtp" || mode == "http" ||
               mode == "hls" || mode == "srt" || mode == "rtsp" ||
               mode == "rtmp";
    }

    explicit InputLivenessPolicy(bool networkSource) noexcept
        : networkSource_(networkSource) {}

    Sample observe(std::uint64_t selectedBytes, std::uint64_t sourceBytes,
                   TimePoint now) noexcept {
        if (!initialized_) {
            initialized_ = true;
            lastMediaAt_ = lastSourceAt_ = now;
        }
        Sample sample;
        // Counters are monotonic within a relay instance. If they wrap or
        // reset, rebase the snapshot without manufacturing false progress.
        sample.mediaAdvanced = selectedBytes > previousMediaBytes_;
        sample.sourceAdvanced = sourceBytes > previousSourceBytes_;
        previousMediaBytes_ = selectedBytes;
        previousSourceBytes_ = sourceBytes;
        if (sample.mediaAdvanced) lastMediaAt_ = now;
        if (sample.sourceAdvanced) lastSourceAt_ = now;

        const auto elapsed = [now](TimePoint since) -> std::uint64_t {
            if (now <= since) return 0ULL;
            return static_cast<std::uint64_t>(
                std::chrono::duration_cast<Milliseconds>(now - since).count());
        };
        sample.mediaSilenceMs = elapsed(lastMediaAt_);
        sample.sourceSilenceMs = elapsed(lastSourceAt_);
        const bool wasStalled = stalled_;

        if (networkSource_) {
            // A delayed HTTP/SRT/UDP producer may deliver bytes long before
            // framing/remap/CA/codec workers consume the selected packets.
            // Allow 10s for actual network silence, but only 18s when raw
            // ingress keeps moving without a valid selected TS packet.
            const bool networkSilent =
                sample.sourceSilenceMs >= 10000ULL;
            const bool mediaExhausted =
                sample.mediaSilenceMs >= 18000ULL;
            stalled_ = !sample.mediaAdvanced &&
                sample.mediaSilenceMs >= 10000ULL &&
                (networkSilent || mediaExhausted);
            sample.jitterGrace = !stalled_ &&
                sample.mediaSilenceMs >= 5000ULL;
            sample.cause = stalled_
                ? (networkSilent ? "network_and_media_silent"
                                 : "network_alive_media_stalled")
                : (sample.jitterGrace ? "network_jitter_grace" : "healthy");
        } else {
            // Legacy 5s guard for DVB, files and tests. The transport socket
            // cannot override missing selected-service data on shared DVB.
            stalled_ = !sample.mediaAdvanced &&
                sample.mediaSilenceMs >= 5000ULL;
            sample.cause = stalled_ ? "selected_media_silent" : "healthy";
        }
        sample.stalled = stalled_;
        sample.recovered = wasStalled && sample.mediaAdvanced;
        return sample;
    }

private:
    bool networkSource_ = false;
    bool initialized_ = false;
    bool stalled_ = false;
    std::uint64_t previousMediaBytes_ = 0;
    std::uint64_t previousSourceBytes_ = 0;
    TimePoint lastMediaAt_{};
    TimePoint lastSourceAt_{};
};

} // namespace dvbstreamer5::media::network
