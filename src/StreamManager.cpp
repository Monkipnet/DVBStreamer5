#include "StreamManager.h"
#include "StreamAuthBackend.h"
#include "StreamAuthSessions.h"

#include "CardManager.h"
#include "CaBackend.h"
#include "DvbSatellite.h"
#include "TranscoderModule.h"
#include "AutoTranscodePolicy.h"
#include "media/IpCbrSourceClockPolicy.h"
#include "media/DvbSourcePcrPacingPolicy.h"
#include "media/WisiChameleonProfilePolicy.h"
#include "mpts/MptsOutputManager.h"
#include "protocols/SrtVpsProfile.h"
#include "protocols/stream/inputs/StreamVdrInputProtocol.h"
#include "utils.h"
#include "media/NativeSampleAes.h"
#include "media/MpegTsRemapper.h"
#include "ExternalXmltv.h"
#include "DvbEitOutput.h"
#include "DvbSubtitlePassthrough.h"
#include "RestartScheduleClock.h"
#include "AutoStartRecoveryPolicy.h"
#include "media/NativeTsDemux.h"
#include "media/HlsSegmentSettings.h"
#include "media/HlsOnlyCbrRoutingPolicy.h"
#include "media/OnDemandHlsStartupPolicy.h"
#include "NativeHttpClient.h"
#include "media/UdpSocket.h"
#include "media/HttpTsEpollDispatcher.h"
#include "media/TransportStream.h"
#include "media/NativeFailoverHealth.h"
#include "media/InputLivenessPolicy.h"
#include "media/RtpMpegTs.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <functional>
#include <sstream>
#include <poll.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <linux/tcp.h>
#include <sys/ioctl.h>
#include <linux/sockios.h>
#include <sys/socket.h>
#include <unistd.h>

// Stream orchestration layer. It owns per-channel runtime state, selects native input
// and output transports, wires CA/remap/transcode/HLS observers, and contains failures
// to one stream. Protocol/socket details stay in src/media and src/protocols.

namespace {

constexpr auto kAdHocSessionTtl = std::chrono::minutes(2);

std::string cleanInterface(std::string value) {
    if (value == "auto" || value == "Auto" || value == "0.0.0.0") return {};
    return value;
}

bool startsWith(const std::string& value, const char* prefix) {
    return value.rfind(prefix, 0) == 0;
}

std::string telegramEscape(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (char ch : value) {
        switch (ch) {
            case '&': escaped += "&amp;"; break;
            case '<': escaped += "&lt;"; break;
            case '>': escaped += "&gt;"; break;
            case '"': escaped += "&quot;"; break;
            default: escaped.push_back(ch); break;
        }
    }
    return escaped;
}

bool telegramUsesEnglish(const ConfigManager& manager) {
    return toLower(manager.config.language) == "en";
}

std::string telegramText(const ConfigManager& manager,
                         const char* ru, const char* en) {
    return telegramUsesEnglish(manager) ? en : ru;
}

std::string telegramStreamName(const StreamConfig& cfg) {
    return cfg.name.empty() ? cfg.id : cfg.name;
}

void sendTelegramStreamState(TelegramNotifier& notifier,
                             const ConfigManager& manager,
                             const StreamConfig& cfg,
                             const std::string& color,
                             const std::string& title,
                             const std::string& details) {
    const std::string serverName = manager.config.serverName.empty()
        ? "DVBStreamer5"
        : manager.config.serverName;
    const bool english = telegramUsesEnglish(manager);
    std::ostringstream message;
    message << color << " <b>" << telegramEscape(title) << "</b>\n"
            << (english ? "Server" : "Сервер") << ": <b>"
            << telegramEscape(serverName) << "</b>\n"
            << (english ? "Channel" : "Канал") << ": <b>"
            << telegramEscape(telegramStreamName(cfg)) << "</b>\n"
            << "ID: <code>" << telegramEscape(cfg.id) << "</code>";
    if (!details.empty()) message << "\n" << telegramEscape(details);
    notifier.sendMessage(message.str());
}

const char* browserPreviewCodecName(
    dvbstreamer5::media::mpegts::ElementaryCodec codec) {
    using dvbstreamer5::media::mpegts::ElementaryCodec;
    switch (codec) {
        case ElementaryCodec::Mpeg2Video: return "mpeg2video";
        case ElementaryCodec::H264: return "h264";
        case ElementaryCodec::H265: return "h265";
        case ElementaryCodec::AacAdts: return "aac-adts";
        case ElementaryCodec::AacLatm: return "aac-latm";
        case ElementaryCodec::MpegAudio: return "mpeg-audio";
        case ElementaryCodec::Ac3: return "ac3";
        case ElementaryCodec::Eac3: return "eac3";
        default: return "unknown";
    }
}

class BrowserPreviewCodecSelector {
public:
    enum class Mode { Detecting, Passthrough, Transcode };

    BrowserPreviewCodecSelector() {
        demux_.setProgramCallback(
            [this](const std::vector<dvbstreamer5::media::mpegts::DemuxStreamInfo>& streams) {
                onProgram(streams);
            });
        demux_.setSampleCallback(
            [](dvbstreamer5::media::mpegts::DemuxSample&&) {});
    }

    Mode inspect(const std::uint8_t* data, std::size_t size, std::string& error) {
        error.clear();
        if (mode_ != Mode::Detecting || !data || size == 0) return mode_;
        if (!demux_.push(data, size, error)) {
            if (error.empty()) error = "preview codec detection failed";
            mode_ = Mode::Transcode;
        }
        return mode_;
    }

    bool takeResolvedLog() {
        if (mode_ == Mode::Detecting || resolvedLogged_) return false;
        resolvedLogged_ = true;
        return true;
    }

    const char* videoCodecName() const {
        return browserPreviewCodecName(videoCodec_);
    }

    const char* audioCodecName() const {
        return hasAudio_ ? browserPreviewCodecName(audioCodec_) : "none";
    }

private:
    void onProgram(
        const std::vector<dvbstreamer5::media::mpegts::DemuxStreamInfo>& streams) {
        using dvbstreamer5::media::mpegts::ElementaryCodec;
        using dvbstreamer5::media::mpegts::ElementaryKind;

        bool haveVideo = false;
        bool haveAudio = false;
        ElementaryCodec videoCodec = ElementaryCodec::Unknown;
        ElementaryCodec audioCodec = ElementaryCodec::Unknown;
        for (const auto& stream : streams) {
            if (!haveVideo && stream.kind == ElementaryKind::Video) {
                haveVideo = true;
                videoCodec = stream.codec;
            } else if (!haveAudio && stream.kind == ElementaryKind::Audio) {
                haveAudio = true;
                audioCodec = stream.codec;
            }
        }
        if (!haveVideo) return;

        videoCodec_ = videoCodec;
        audioCodec_ = audioCodec;
        hasAudio_ = haveAudio;

        // mpegts.js/MediaSource can consume the source TS directly only for the
        // reliable browser combination AVC + ADTS AAC. Other DVB combinations
        // (MPEG-2, HEVC, LATM, MP2, AC-3/E-AC-3) use the H.264/AAC fallback.
        const bool browserVideo = videoCodec == ElementaryCodec::H264;
        const bool browserAudio = !haveAudio || audioCodec == ElementaryCodec::AacAdts;
        mode_ = browserVideo && browserAudio
            ? Mode::Passthrough
            : Mode::Transcode;
    }

    dvbstreamer5::media::mpegts::NativeTsDemux demux_;
    Mode mode_ = Mode::Detecting;
    dvbstreamer5::media::mpegts::ElementaryCodec videoCodec_ =
        dvbstreamer5::media::mpegts::ElementaryCodec::Unknown;
    dvbstreamer5::media::mpegts::ElementaryCodec audioCodec_ =
        dvbstreamer5::media::mpegts::ElementaryCodec::Unknown;
    bool hasAudio_ = false;
    bool resolvedLogged_ = false;
};


class SharedDvbServicePrefilter {
public:
    bool initialize(
        const std::string& streamId,
        const dvbstreamer5::media::mpegts::RemapConfig& config,
        bool dropSourceNullPackets,
        std::string& error) {
        streamId_ = streamId;
        dropSourceNullPackets_ = dropSourceNullPackets;
        filteredPackets_.reserve(128);
        admission_ = std::make_shared<
            dvbstreamer5::media::network::SharedDvbPidAdmission>();
        publishedGeneration_ = 0;
        return remapper_.initialize(config, error);
    }

    bool push(
        dvbstreamer5::media::network::NativeUdpRelay* relay,
        const std::uint8_t* data,
        std::size_t size) {
        if (!relay || !data || size == 0) return false;

        filteredPackets_.clear();
        const auto prefilterStart = std::chrono::steady_clock::now();
        std::string remapError;
        VisitContext visitContext {this, relay, &remapError};

        // V10.8.129: process trusted aligned DVB packets in source order without
        // first copying the whole MPTS into inputPackets_. This preserves the
        // dynamic PAT -> PMT -> ES admission sequence inside a single input
        // chunk: wantsInputPid() observes remapper state updated by every prior
        // admitted packet. Only packets belonging to the selected service are
        // copied into a Packet for full structural validation/remapping.
        if (!framer_.visitTrustedAlignedInline(
                data, size, [&](const std::uint8_t* bytes) {
                    return visitPacket(&visitContext, bytes);
                })) {
            return false;
        }

        // Preserve the V10.8.114 PAT/PMT/SDT cadence even when an input
        // block contained only foreign-service PIDs and therefore skipped every
        // Remapper::process() call above.
        remapper_.tick(filteredPackets_);

        // Publish only when PAT/CAT/PMT changed, not for every 64 KiB read.
        // The immutable mask is consumed by the physical source thread.
        const auto generation = remapper_.inputAdmissionGeneration();
        if (admission_ && publishedGeneration_ != generation) {
            using Mask = dvbstreamer5::media::network::SharedDvbPidAdmission::Mask;
            auto writable = std::make_shared<Mask>();
            for (std::size_t pid = 0; pid < writable->size(); ++pid) {
                // Shared DVB drops source NULL packets in its original
                // prefilter. Never queue MPTS stuffing ahead of that stage.
                (*writable)[pid] = (pid != dvbstreamer5::media::mpegts::kNullPid ||
                                    !dropSourceNullPackets_) &&
                                   remapper_.wantsInputPid(
                                       static_cast<std::uint16_t>(pid)) ? 1U : 0U;
            }
            std::shared_ptr<const Mask> snapshot = std::move(writable);
            admission_->publish(std::move(snapshot));
            publishedGeneration_ = generation;
        }

        const auto prefilterMs = std::chrono::duration_cast<
            std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - prefilterStart).count();
        if (prefilterMs > 50 && logSlow(++slowPrefilterEvents_)) {
            std::cerr << "SHARED DVB PREFILTER SLOW stream=" << streamId_
                      << " input_bytes=" << size
                      << " selected_packets=" << filteredPackets_.size()
                      << " elapsed_ms=" << prefilterMs
                      << " occurrences=" << slowPrefilterEvents_ << std::endl;
        }
        if (filteredPackets_.empty()) return true;
        static_assert(
            sizeof(dvbstreamer5::media::mpegts::Packet) ==
                dvbstreamer5::media::mpegts::kPacketSize,
            "MPEG-TS packet storage must be contiguous");
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(
            filteredPackets_.data());
        const auto pushStart = std::chrono::steady_clock::now();
        const bool accepted = relay->pushInput(
            bytes,
            filteredPackets_.size() * dvbstreamer5::media::mpegts::kPacketSize);
        const auto pushMs = std::chrono::duration_cast<
            std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - pushStart).count();
        if (pushMs > 50 && logSlow(++slowRelayEvents_)) {
            std::cerr << "SHARED DVB RELAY PUSH SLOW stream=" << streamId_
                      << " selected_packets=" << filteredPackets_.size()
                      << " elapsed_ms=" << pushMs
                      << " occurrences=" << slowRelayEvents_
                      << " accepted=" << (accepted ? 1 : 0) << std::endl;
        }
        return accepted;
    }

    std::shared_ptr<dvbstreamer5::media::network::SharedDvbPidAdmission>
    pidAdmission() const noexcept { return admission_; }

private:
    static bool logSlow(std::uint64_t n) noexcept {
        return n <= 3 || (n & (n - 1)) == 0;
    }

    struct VisitContext {
        SharedDvbServicePrefilter* self = nullptr;
        dvbstreamer5::media::network::NativeUdpRelay* relay = nullptr;
        std::string* remapError = nullptr;
    };

    static bool visitPacket(void* opaque, const std::uint8_t* bytes) {
        auto* context = static_cast<VisitContext*>(opaque);
        if (!context || !context->self || !context->relay ||
            !context->remapError || !bytes) {
            return false;
        }

        auto* self = context->self;
        if (bytes[0] == dvbstreamer5::media::mpegts::kSyncByte) {
            const std::uint16_t pid = static_cast<std::uint16_t>(
                (static_cast<std::uint16_t>(bytes[1] & 0x1fU) << 8) |
                bytes[2]);
            if (self->dropSourceNullPackets_ &&
                pid == dvbstreamer5::media::mpegts::kNullPid) {
                return true;
            }
            if (!self->remapper_.wantsInputPid(pid)) return true;
        }

        dvbstreamer5::media::mpegts::Packet packet {};
        std::memcpy(packet.data(), bytes, packet.size());

        dvbstreamer5::media::mpegts::PacketInfo selectedInfo;
        if (!dvbstreamer5::media::mpegts::inspectPacket(
                packet.data(), packet.size(), selectedInfo)) {
            return true;
        }

        if (!self->remapper_.processTrusted(
                packet, selectedInfo, self->filteredPackets_, *context->remapError)) {
            const std::string message = context->remapError->empty()
                ? "shared DVB service prefilter failed"
                : "shared DVB service prefilter failed: " + *context->remapError;
            std::cerr << "SHARED DVB service prefilter failed stream="
                      << self->streamId_ << " error=" << message << std::endl;
            context->relay->finishInput(message);
            return false;
        }
        return true;
    }

    std::string streamId_;
    bool dropSourceNullPackets_ = false;
    std::shared_ptr<dvbstreamer5::media::network::SharedDvbPidAdmission> admission_;
    std::uint64_t publishedGeneration_ = 0;
    std::uint64_t slowPrefilterEvents_ = 0;
    std::uint64_t slowRelayEvents_ = 0;
    dvbstreamer5::media::mpegts::PacketFramer framer_;
    dvbstreamer5::media::mpegts::Remapper remapper_;
    std::vector<dvbstreamer5::media::mpegts::Packet> filteredPackets_;
};

struct AbrProfile {
    std::string name;
    int width = 0;
    int height = 0;
    std::uint64_t videoBitrate = 0;
};

int alignAbrDimension8(int value) {
    // Native HEVC/Kvazaar requires coded dimensions divisible by 8.  Keep
    // every generated ABR rendition on that boundary so switching the video
    // codec between H.264/H.265 or CPU/hardware backends cannot make one
    // ladder entry fail during startup (notably the traditional 854x480
    // profile).  Round to the nearest multiple of eight, with a minimum of 8.
    if (value <= 8) return 8;
    return std::max(8, ((value + 4) / 8) * 8);
}

std::vector<AbrProfile> makeAbrProfiles(int primaryWidth, int primaryHeight,
                                        std::uint64_t primaryVideoBitrate) {
    struct Candidate { const char* name; int width; int height; std::uint64_t nominal; };
    static constexpr Candidate candidates[] = {
        {"1080p", 1920, 1080, 6500000ULL},
        {"720p", 1280, 720, 3500000ULL},
        {"480p", 854, 480, 1800000ULL},
        {"360p", 640, 360, 950000ULL},
    };
    std::vector<AbrProfile> out;
    if (primaryWidth <= 0 || primaryHeight <= 0 || primaryVideoBitrate < 350000ULL)
        return out;

    const std::uint64_t primaryPixels = static_cast<std::uint64_t>(primaryWidth) *
        static_cast<std::uint64_t>(primaryHeight);
    std::uint64_t previousBitrate = primaryVideoBitrate;
    constexpr std::uint64_t kMinimumVariantBitrate = 250000ULL;

    for (const auto& c : candidates) {
        if (out.size() >= 3 || previousBitrate <= kMinimumVariantBitrate + 50000ULL) break;
        const int width = alignAbrDimension8(c.width);
        const int height = alignAbrDimension8(c.height);
        if (width >= primaryWidth || height >= primaryHeight) continue;
        const std::uint64_t pixels = static_cast<std::uint64_t>(width) *
            static_cast<std::uint64_t>(height);
        if (pixels >= primaryPixels) continue;

        std::uint64_t scaled = (primaryVideoBitrate * pixels * 135ULL) /
            (primaryPixels * 100ULL);
        scaled = std::min<std::uint64_t>(scaled, c.nominal);
        // Keep a real bitrate step between adjacent renditions.  The old ladder
        // was calculated from the UI-requested bitrate and could create a
        // "lower" rendition that was actually more expensive than the effective
        // CBR-limited primary stream.
        scaled = std::min<std::uint64_t>(scaled, previousBitrate * 80ULL / 100ULL);
        scaled = std::max<std::uint64_t>(scaled, kMinimumVariantBitrate);
        if (scaled + 50000ULL >= previousBitrate) continue;

        out.push_back({c.name, width, height, scaled});
        previousBitrate = scaled;
    }
    return out;
}

std::filesystem::path hlsRuntimeDirectory(const StreamConfig& cfg) {
    return cfg.hlsArchiveEnabled
        ? std::filesystem::path(cfg.hlsArchivePath) / cfg.id
        : std::filesystem::path("/tmp/dvbstreamer5-hls") / cfg.id;
}

bool writeAbrMasterPlaylist(const StreamConfig& cfg, int primaryWidth, int primaryHeight,
                            std::uint64_t primaryVideoBitrate,
                            std::uint64_t primaryTransportBitrate,
                            const std::vector<std::unique_ptr<StreamState::HlsAbrVariantRuntime>>& variants,
                            std::string& error) {
    const auto dir = hlsRuntimeDirectory(cfg);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) { error = "cannot create HLS ABR directory: " + ec.message(); return false; }
    const auto tmp = dir / "master.m3u8.tmp";
    const auto dst = dir / "master.m3u8";
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) { error = "cannot create HLS ABR master playlist"; return false; }
    const std::uint64_t audio = cfg.transcodeAudioCodec == "copy" ? 192000ULL : cfg.transcodeAudioBitrate;
    const std::string abrAudioCodec = toLower(cfg.transcodeAudioCodec);
    const bool advertiseCodecs = abrAudioCodec == "aac";
    auto codecs = [&cfg, &abrAudioCodec]() {
        const std::string v = toLower(cfg.transcodeVideoCodec);
        std::string c = (v == "hevc" || v == "h265") ? "hvc1.1.6.L120.B0" : "avc1.640028";
        if (abrAudioCodec == "aac") c += ",mp4a.40.2";
        return c;
    }();
    auto emit = [&](int w, int h, std::uint64_t vb, std::uint64_t transportBitrate,
                    const std::string& uri) {
        const std::uint64_t estimated = std::max<std::uint64_t>(350000ULL, vb + audio + 180000ULL);
        const std::uint64_t bandwidth = transportBitrate > 0 ? transportBitrate : estimated;
        const std::uint64_t average = transportBitrate > 0
            ? transportBitrate
            : std::max<std::uint64_t>(250000ULL, vb + audio);
        out << "#EXT-X-STREAM-INF:BANDWIDTH=" << bandwidth
            << ",AVERAGE-BANDWIDTH=" << std::min(bandwidth, average)
            << ",RESOLUTION=" << w << "x" << h
            << ",FRAME-RATE=25.000";
        // For audio=copy/MP2 the source codec is learned only from the live PMT.
        // Advertising a video-only CODECS list tells some HLS clients that the
        // rendition has no audio.  Omit the optional attribute unless the full
        // A/V codec list is known (AAC transcode); clients then probe the TS PMT.
        if (advertiseCodecs) out << ",CODECS=\"" << codecs << "\"";
        out << "\n";
        out << uri << "\n";
    };
    out << "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-INDEPENDENT-SEGMENTS\n# DVBStreamer5 native ABR\n";
    emit(primaryWidth, primaryHeight, primaryVideoBitrate, primaryTransportBitrate, "video.m3u8");
    for (const auto& v : variants) {
        if (!v || !v->enabled || v->failed) continue;
        emit(v->width, v->height, v->videoBitrate, v->muxBitrate,
             "abr/" + v->name + "/video.m3u8");
    }
    out.flush();
    if (!out) { error = "failed to write HLS ABR master playlist"; return false; }
    out.close();
    std::filesystem::rename(tmp, dst, ec);
    if (ec) {
        std::filesystem::remove(dst, ec); ec.clear();
        std::filesystem::rename(tmp, dst, ec);
    }
    if (ec) { error = "cannot publish HLS ABR master playlist: " + ec.message(); return false; }
    return true;
}

} // namespace

StreamManager::StreamManager(ConfigManager& cfg, TelegramNotifier& notifier)
    : configManager(cfg), telegramNotifier(notifier),
      mptsOutputManager(std::make_unique<MptsOutputManager>()),
      externalXmltvManager_(std::make_unique<ExternalXmltvManager>(cfg.config.xmltvSources)) {
    // V397: 4 epoll pollers replace one detached writer thread per viewer.
    httpEpollDispatcher_=std::make_unique<
        dvbstreamer5::media::network::HttpTsEpollDispatcher>(4);
    configureMptsOutputs();
    // Periodic cache writes stay off the native MPEG-TS processing thread.
    epgPersistThread_ = std::thread([this] {
        while (!epgPersistStop_.load(std::memory_order_acquire)) {
            {
                std::unique_lock<std::mutex> lock(epgPersistMutex_);
                epgPersistCv_.wait_for(lock, std::chrono::seconds(60),
                    [this] { return epgPersistStop_.load(std::memory_order_acquire); });
            }
            if (!epgPersistStop_.load(std::memory_order_acquire))
                persistAllDvbEpg();
        }
    });
    onDemandMonitorThread = std::thread(&StreamManager::monitorOnDemandStreams, this);
    failoverMonitorThread = std::thread(&StreamManager::monitorFailoverStreams, this);
    configureRestartSchedule();
    restartScheduleThread = std::thread(&StreamManager::monitorGlobalRestartSchedule, this);
    // Independent stop workers: a hung codec/source on one tile must not
    // indefinitely block Stop requests for all other tiles.
    for (int i = 0; i < 3; ++i)
        asyncStopThreads_.emplace_back(&StreamManager::processAsyncStopRequests, this);
    configureAutoStartRecovery();
    // Independent lanes: a stalled input teardown cannot block other tiles.
    for (unsigned i = 0; i < 3; ++i)
        autoRecoveryThreads_.emplace_back(&StreamManager::monitorAutoStartRecovery, this, i);
}

StreamManager::~StreamManager() {
    autoRecoveryStop_.store(true, std::memory_order_release);
    for (auto& worker : autoRecoveryThreads_) if (worker.joinable()) worker.join();
    failoverMonitorStop.store(true, std::memory_order_release);
    if (failoverMonitorThread.joinable()) failoverMonitorThread.join();
    {
        std::lock_guard<std::mutex> lock(asyncStopMutex_);
        asyncStopShutdown_ = true;
    }
    asyncStopCv_.notify_all();
    for (auto& worker : asyncStopThreads_) if (worker.joinable()) worker.join();
    restartScheduleStop.store(true, std::memory_order_release);
    restartScheduleCv.notify_all();
    if (restartScheduleThread.joinable()) restartScheduleThread.join();
    epgPersistStop_.store(true, std::memory_order_release);
    epgPersistCv_.notify_all();
    if (epgPersistThread_.joinable()) epgPersistThread_.join();
    persistAllDvbEpg(); // flush before stopping running DVB feeds
    onDemandMonitorStop.store(true, std::memory_order_release);
    if (onDemandMonitorThread.joinable()) onDemandMonitorThread.join();
    // Close HTTP clients and join their shared pollers before destroying
    // stream state. Completion callbacks can still access managerMutex.
    if(httpEpollDispatcher_)httpEpollDispatcher_->stop();
    stopAll();
}

std::string StreamManager::normalizedOutputType(const StreamConfig& cfg,
                                                const StreamOutputConfig* extra) {
    std::string type = toLower(extra ? extra->outputType : cfg.outputType);
    if (type == "udp_cbr" || type == "udpcbr") type = "udp-cbr";
    if (type == "udp_vbr" || type == "udpvbr") type = "udp-vbr";
    if (type == "udp") type = extra ? "udp-vbr" : (cfg.cbr ? "udp-cbr" : "udp-vbr");
    return type;
}

bool StreamManager::isNativeInputSupported(const StreamConfig& cfg, std::string& reason) {
    const std::string normalizedInput = normalizeInputUri(cfg.inputUri);
    const std::string input = toLower(normalizedInput);
    const std::string mode = toLower(cfg.inputMode);
    if (cfg.testPattern || mode == "test" || input == "test://bars" || input == "testsrc://bars" || input == "bars://hd") {
        return true;
    }
    if (dvbstreamer5::stream_protocols::inputs::isVdrInput(
            input, mode, cfg.testPattern)) {
        std::string resolved;
        if (!dvbstreamer5::stream_protocols::inputs::resolveVdrStreamdevUri(
                normalizedInput, resolved, reason)) {
            return false;
        }
        return true;
    }
    if (startsWith(input, "udp://") || startsWith(input, "rtp://") ||
        startsWith(input, "http://") || startsWith(input, "https://") ||
        startsWith(input, "file://") || startsWith(input, "srt://") ||
        startsWith(input, "rtsp://") || startsWith(input, "rtmp://") || startsWith(input, "rtmps://") || DvbSatellite::isDvbUri(cfg.inputUri) ||
        input.find("://") == std::string::npos) {
        return true;
    }
    reason = "unsupported native input protocol";
    return false;
}

bool StreamManager::isNativeOutputSupported(const std::string& type, std::string& reason) {
    if (type == "udp-cbr" || type == "udp-vbr" || type == "rtp" || type == "http" || type == "hls" || type == "srt" ||
        type == "rtsp" || type == "rtmp" || type == "youtube") return true;
    reason = "unsupported native output protocol";
    return false;
}

bool StreamManager::startStream(const StreamConfig& streamConfig, std::string* error) {
    if (error) error->clear();
    if (streamConfig.id.empty()) {
        if (error) *error = "stream id is empty";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(startingStreamsMutex_);
        if (!startingStreamIds_.insert(streamConfig.id).second) {
            if (error) *error = "stream start already in progress";
            return false;
        }
    }
    struct LaunchGuard {
        std::mutex& mutex;
        std::set<std::string>& ids;
        const std::string& id;
        ~LaunchGuard() {
            std::lock_guard<std::mutex> lock(mutex);
            ids.erase(id);
        }
    } launchGuard{startingStreamsMutex_, startingStreamIds_, streamConfig.id};
    // Do not resurrect a tile stopped by the operator. A queued async Stop
    // must win over automatic failover, scheduled restart and OnDemand start.
    {
        std::lock_guard<std::mutex> lock(asyncStopMutex_);
        if (asyncStopPending_.count(streamConfig.id) ||
            operatorStoppedStreams_.count(streamConfig.id)) {
            if (error) *error = "stream has an operator stop request";
            return false;
        }
    }
    std::string reason;
    if (!isNativeInputSupported(streamConfig, reason)) {
        if (error) *error = reason;
        return false;
    }

    const std::string configuredInputUri = normalizeInputUri(streamConfig.inputUri);
    const std::string configuredInputMode = toLower(streamConfig.inputMode);
    const bool vdrCiPlusInput =
        dvbstreamer5::stream_protocols::inputs::isVdrInput(
            toLower(configuredInputUri), configuredInputMode, streamConfig.testPattern);
    std::string resolvedInputUri = configuredInputUri;
    if (vdrCiPlusInput) {
        if (!dvbstreamer5::stream_protocols::inputs::resolveVdrStreamdevUri(
                configuredInputUri, resolvedInputUri, reason)) {
            if (error) *error = reason;
            return false;
        }
        std::cerr << "CI+ VDR bridge input"
                  << " stream=" << streamConfig.id
                  << " source=" << configuredInputUri
                  << " resolved=" << resolvedInputUri
                  << " internal_ca=disabled"
                  << std::endl;
    }

    std::vector<dvbstreamer5::media::network::NativeUdpRelayOutputConfig> nativeOutputs;
    struct SrtOutputSpec {
        std::string host; int port = 0; std::string mode; std::string iface;
        int latency = 120; std::string passphrase; std::string streamId; int pbkeylen = 16;
    };
    std::vector<SrtOutputSpec> srtOutputSpecs;
    struct RtspOutputSpec { std::string host; int port=8554; std::string iface; };
    std::vector<RtspOutputSpec> rtspOutputSpecs;
    struct RtmpOutputSpec { std::string uri; std::string iface; };
    std::vector<RtmpOutputSpec> rtmpOutputSpecs;
    bool hasHttpOutput = false;
    bool hasHlsOutput = false;
    auto appendOutput = [&](const std::string& type, const std::string& host, int port,
                            const std::string& iface, const std::string& mode,
                            int srtLatency, const std::string& srtPassphrase,
                            const std::string& srtStreamId, int srtPbKeyLen) -> bool {
        std::string outputReason;
        if (!isNativeOutputSupported(type, outputReason)) {
            if (error) *error = outputReason;
            return false;
        }
        if (type == "http") { hasHttpOutput = true; return true; }
        if (type == "hls") { hasHlsOutput = true; return true; }
        if (type == "srt") {
            srtOutputSpecs.push_back({host, port, mode.empty() ? "listener" : mode, iface,
                                      srtLatency, srtPassphrase, srtStreamId, srtPbKeyLen});
            return true;
        }
        if (type == "rtsp") { rtspOutputSpecs.push_back({host, port > 0 ? port : 8554, cleanInterface(iface)}); return true; }
        if (type == "rtmp" || type == "youtube") {
            std::string uri = host;
            const std::string lo = toLower(uri);
            if (type == "youtube" && lo.rfind("rtmp",0) != 0) uri = "rtmp://a.rtmp.youtube.com/live2/" + host;
            else if (type == "rtmp" && lo.rfind("rtmp",0) != 0) uri = "rtmp://" + host + ":" + std::to_string(port > 0 ? port : 1935) + "/live/" + streamConfig.id;
            rtmpOutputSpecs.push_back({uri, cleanInterface(iface)}); return true;
        }
        nativeOutputs.push_back({type, host, port, cleanInterface(iface)});
        return true;
    };

    if (!appendOutput(normalizedOutputType(streamConfig), streamConfig.outputHost,
                      streamConfig.outputPort, streamConfig.interfaceAddress, streamConfig.outputMode,
                      streamConfig.srtOutputLatencyMs, streamConfig.srtOutputPassphrase,
                      streamConfig.srtOutputStreamId, streamConfig.srtOutputPbKeyLen)) return false;
    for (const auto& extra : streamConfig.additionalOutputs) {
        if (!appendOutput(normalizedOutputType(streamConfig, &extra), extra.outputHost,
                          extra.outputPort, extra.interfaceAddress, extra.outputMode,
                          extra.srtLatencyMs, extra.srtPassphrase, extra.srtStreamId, extra.srtPbKeyLen)) return false;
    }

    bool staleStreamState = false;
    {
        std::lock_guard<std::mutex> lock(managerMutex);
        const auto existing = streams.find(streamConfig.id);
        if (existing != streams.end()) {
            if (existing->second && existing->second->active.load()) {
                if (error) *error = "stream is already active";
                return false;
            }
            // monitorNativeStream marks a failed/stopped relay inactive but the
            // StreamState remains in the map until stopStream() owns teardown.
            // Treat such an OFFLINE entry as stale so the UI Start button can
            // actually start it again instead of returning "already active".
            staleStreamState = true;
        }
    }
    if (staleStreamState) {
        (void)stopStream(streamConfig.id);
    }

    std::string caError;
    if (!vdrCiPlusInput &&
        !CardManager::instance().reserveService(streamConfig, &caError)) {
        if (error) *error = caError.empty() ? "CAM reservation failed" : caError;
        return false;
    }

    auto state = std::make_unique<StreamState>();
    std::uint64_t effectivePrimaryVideoBitrate = streamConfig.transcodeVideoBitrate;
    state->config = streamConfig;
    state->activeInputUri = resolvedInputUri;
    {
        std::lock_guard<std::mutex> failLock(failoverMutex);
        const auto it = failoverEntries.find(streamConfig.id);
        if (it != failoverEntries.end() && it->second.switching)
            state->usingBackup = it->second.backupActive;
    }
    // V348: reuse one public HTTP MPEG-TS hub across input transitions.
    // Private preview keeps its own fresh, per-source hub.
    const bool configuredFailover = !streamConfig.backupInputUri.empty() &&
        streamConfig.backupInputUri != streamConfig.inputUri;
    bool automaticSourceSwitch = false;
    {
        std::lock_guard<std::mutex> lock(failoverMutex);
        const auto it = failoverEntries.find(streamConfig.id);
        automaticSourceSwitch =
            it != failoverEntries.end() && it->second.switching;
    }
    if (configuredFailover) {
        std::lock_guard<std::mutex> lock(managerMutex);
        auto& hub = failoverHttpHubs[streamConfig.id];
        if (!hub)
            hub = std::make_shared<
                dvbstreamer5::media::network::NativePreviewHub>();
        state->nativeHttpHub = hub;
    } else {
        state->nativeHttpHub = std::make_shared<
            dvbstreamer5::media::network::NativePreviewHub>();
    }
    state->nativePreviewHub = std::make_shared<dvbstreamer5::media::network::NativePreviewHub>();
    state->nativeRelay = std::make_shared<dvbstreamer5::media::network::NativeUdpRelay>();

    // V10.8.83: browser preview is adaptive. Do not start decoder/encoder
    // worker threads for streams that mpegts.js can consume directly. The
    // H.264/AAC transcoder is constructed lazily only after PMT probing says
    // that the source A/V combination is not browser-safe.
    dvbstreamer5::media::transcode::NativeTranscoderConfig previewTc;
    previewTc.videoCodec = "h264";
    previewTc.videoEncoder = "auto";
    previewTc.audioCodec = "aac";
    previewTc.width = 1280;
    previewTc.height = 720;
    previewTc.fps = 25.0;
    previewTc.videoBitrate = 1800000ULL;
    previewTc.audioBitrate = 128000ULL;
    previewTc.deinterlace = true;
    previewTc.lockOutputGeometry = true;
    previewTc.serviceId = static_cast<std::uint16_t>(
        streamConfig.serviceId > 0 && streamConfig.serviceId <= 0xffff
            ? streamConfig.serviceId
            : (streamConfig.inputServiceId > 0 && streamConfig.inputServiceId <= 0xffff
                ? streamConfig.inputServiceId : 1));
    previewTc.videoPid = 0x0100;
    previewTc.audioPid = 0x0101;
    previewTc.muxBitrate = 0;
    previewTc.serviceName =
        streamConfig.serviceName.empty() ? streamConfig.name : streamConfig.serviceName;
    previewTc.serviceProvider =
        streamConfig.serviceProvider.empty() ? "DVBStreamer5" : streamConfig.serviceProvider;
    std::cerr << "NATIVE BROWSER PREVIEW adaptive ready stream="
              << streamConfig.name
              << " direct=h264/aac fallback=h264/aac"
              << " size=1280x720 encoder=auto"
              << " policy=nvenc->qsv-vaapi->cpu"
              << std::endl;

    // V518: WISI encoder defaults are automatic for transcoded UDP outputs.
    // Copy, non-UDP and browser-preview paths retain their old semantics.
    const auto outputVideoCodec = toLower(streamConfig.transcodeVideoCodec);
    const auto hasPhysicalCbr = std::any_of(
        nativeOutputs.begin(), nativeOutputs.end(),
        [](const auto& out) { return out.outputType == "udp-cbr"; });
    const auto hasPhysicalVbr = std::any_of(
        nativeOutputs.begin(), nativeOutputs.end(),
        [](const auto& out) { return out.outputType == "udp-vbr"; });
    bool wisiCbrProfileActive =
        dvbstreamer5::media::codec::WisiChameleonProfilePolicy::cbrEligible(
            streamConfig.transcodeEnabled, streamConfig.cbr,
            hasPhysicalCbr, outputVideoCodec);
    const bool wisiVbrProfileActive =
        dvbstreamer5::media::codec::WisiChameleonProfilePolicy::vbrEligible(
            streamConfig.transcodeEnabled, hasPhysicalVbr,
            outputVideoCodec);
    std::uint64_t wisiVbrPeakBitsPerSecond = 0ULL;
    bool wisiChameleonProfileActive =
        wisiCbrProfileActive || wisiVbrProfileActive;

    if (streamConfig.transcodeEnabled) {
        state->nativeTranscoder = std::make_unique<dvbstreamer5::media::transcode::NativeTranscoderPipeline>();
        dvbstreamer5::media::transcode::NativeTranscoderConfig tc;
        tc.streamId = streamConfig.id;
        tc.videoCodec = toLower(streamConfig.transcodeVideoCodec);
        tc.videoEncoder = toLower(streamConfig.transcodeVideoEncoder);
        tc.videoDecoder = toLower(streamConfig.transcodeVideoDecoder);
        tc.audioCodec = toLower(streamConfig.transcodeAudioCodec);
        if (!TranscoderModule::resolutionSize(streamConfig.transcodeResolution, tc.width, tc.height)) {
            CardManager::instance().releaseService(streamConfig.id);
            if (error) *error = "invalid native transcode resolution: " + streamConfig.transcodeResolution;
            return false;
        }
        tc.audioBitrate = streamConfig.transcodeAudioBitrate;
        tc.videoBitrate = streamConfig.transcodeVideoBitrate;
        tc.fps = streamConfig.transcodeFps;
        // V487: the per-tile HTTP TS Jitter Buffer opts 25-fps transcode
        // into bounded PES pacing. DVB/SRT/copy/other FPS remain unchanged.
        const std::string pesPacerSource = toLower(resolvedInputUri);
        tc.videoPesPacerEnabled =
            streamConfig.httpTsInputJitterEnabled &&
            (startsWith(pesPacerSource, "http://") ||
             startsWith(pesPacerSource, "https://")) &&
            tc.videoCodec != "copy" &&
            tc.fps >= 24.99 && tc.fps <= 25.01;
        if (tc.videoPesPacerEnabled) {
            std::cerr << "NATIVE VIDEO PES PACER enabled stream_id="
                      << streamConfig.id
                      << " opt_in=http_ts_jitter"
                      << " step_ms=40 wait_budget_ms=200"
                      << " raw_high_watermark=8 raw_low_watermark=5"
                      << std::endl;
        }
        tc.deinterlace = streamConfig.transcodeDeinterlace;
        tc.deinterlaceMode = streamConfig.transcodeDeinterlaceMode;
        // The requested transcoding video bitrate (NOT MPEG-TS transport
        // CBR target) and chosen FPS/resolution drive automatic encoder tuning.
        // Preserve that input even when downstream CBR headroom limits the
        // effective encoder bitrate. Legacy advanced settings are ignored.
        const std::uint64_t requestedTranscodeVideoBitrate = tc.videoBitrate;
        // A CBR transport cannot sustainably carry an elementary-video target
        // equal to the complete TS target: audio, PES/TS headers, PSI/SI and
        // encoder overshoot also consume bitrate.  When transcoding to CBR,
        // reserve explicit mux headroom and cap only the *effective* encoder
        // target.  Keep the user's configured value unchanged on disk/UI.
        if (streamConfig.cbr && streamConfig.targetBitrate >= 1000000ULL &&
            tc.videoCodec != "copy") {
            const std::uint64_t percentageHeadroom = streamConfig.targetBitrate / 20ULL; // 5%
            const std::uint64_t reserve = std::max<std::uint64_t>(
                500000ULL, tc.audioBitrate + percentageHeadroom);
            const std::uint64_t videoBudget = streamConfig.targetBitrate > reserve
                ? streamConfig.targetBitrate - reserve
                : streamConfig.targetBitrate / 2ULL;
            const std::uint64_t effectiveVideo = std::max<std::uint64_t>(500000ULL,
                std::min<std::uint64_t>(tc.videoBitrate, videoBudget));
            if (effectiveVideo != tc.videoBitrate) {
                std::cerr << "NATIVE TRANSCODER CBR BUDGET stream=" << streamConfig.name
                          << " target_kbps=" << (streamConfig.targetBitrate / 1000ULL)
                          << " requested_video_kbps=" << (tc.videoBitrate / 1000ULL)
                          << " effective_video_kbps=" << (effectiveVideo / 1000ULL)
                          << " audio_kbps=" << (tc.audioBitrate / 1000ULL)
                          << " reserve_kbps=" << (reserve / 1000ULL)
                          << std::endl;
            }
            tc.videoBitrate = effectiveVideo;
        }
        if (wisiCbrProfileActive) {
            const auto transportBps =
                dvbstreamer5::media::mpegts::CbrTsPacer::decodeTargetBitrate(
                    streamConfig.targetBitrate);
            if (!dvbstreamer5::media::codec::WisiChameleonProfilePolicy::
                    hasTransportHeadroom(transportBps, tc.videoBitrate,
                                         tc.audioBitrate)) {
                wisiCbrProfileActive = false;
                std::cerr << "NATIVE WISI ENCODER PROFILE inactive stream_id="
                          << streamConfig.id
                          << " reason=insufficient-ts-audio-psi-headroom"
                          << " target_kbps=" << transportBps / 1000ULL
                          << " video_kbps=" << tc.videoBitrate / 1000ULL
                          << " audio_kbps=" << tc.audioBitrate / 1000ULL
                          << std::endl;
            }
        }
        wisiChameleonProfileActive =
            wisiCbrProfileActive || wisiVbrProfileActive;
        if (wisiVbrProfileActive) {
            wisiVbrPeakBitsPerSecond =
                dvbstreamer5::media::codec::WisiChameleonProfilePolicy::
                    vbrPeakBitsPerSecond(tc.videoBitrate, tc.audioBitrate);
        }
        tc.wisiChameleonProfile = wisiChameleonProfileActive;
        if (tc.wisiChameleonProfile) {
            std::cerr << "NATIVE WISI ENCODER PROFILE active stream_id="
                      << streamConfig.id
                      << " mode=" << (wisiCbrProfileActive ? "cbr" : "vbr")
                      << " policy=automatic"
                      << " codec=" << tc.videoCodec
                      << " encoder=" << tc.videoEncoder
                      << " video_kbps=" << tc.videoBitrate / 1000ULL
                      << " cpb_window_ms=400"
                      << " pcr_interval_ms=" << (wisiCbrProfileActive ? 20 : 40)
                      << " vbr_peak_kbps=" << wisiVbrPeakBitsPerSecond / 1000ULL
                      << " experimental=1" << std::endl;
        }
        // V10.8.240: encoder tuning follows the user-selected TRANSCODING
        // video bitrate, independent of the separate transport CBR setting.
        const auto autoTc = dvbstreamer5::app::automaticTranscodeSettings(
            tc.fps, requestedTranscodeVideoBitrate,
            static_cast<std::uint32_t>(tc.width),
            static_cast<std::uint32_t>(tc.height), tc.videoEncoder);
        tc.preset = autoTc.preset;
        // Intel-only override. Never alter NVENC or CPU settings.
        // Auto on Intel favours quality when upscaling or allocating
        // fewer bits per pixel at the requested output frame rate.
        if (tc.videoEncoder == "qsv" || tc.videoEncoder == "vaapi") {
            if (streamConfig.intelPresetMode == "balanced" ||
                streamConfig.intelPresetMode == "quality") {
                tc.preset = streamConfig.intelPresetMode;
            } else {
                const double pixelsPerSecond =
                    static_cast<double>(tc.width) * tc.height * tc.fps;
                const double bpp = pixelsPerSecond > 0
                    ? static_cast<double>(tc.videoBitrate) / pixelsPerSecond : 1.0;
                tc.preset = (tc.width >= 1920 || bpp < 0.15)
                    ? "quality" : "balanced";
            }
        }
        tc.profile = autoTc.profile;
        // V300: honour the validated H.264 profile selected in the tile.
        // Automatic tuning must not overwrite the explicit Main/High choice.
        if (tc.videoCodec == "h264" &&
            (streamConfig.transcodeProfile == "main" ||
             streamConfig.transcodeProfile == "high")) {
            tc.profile = streamConfig.transcodeProfile;
        }
        tc.level = autoTc.level;
        tc.gopFrames = autoTc.gopFrames;
        tc.bFrames = autoTc.bFrames;
        tc.refFrames = autoTc.refFrames;
        // V302: opt-in Intel H.264 DPB reference-count A/B; default unchanged.
        // Never alter NVENC, CPU, or non-H.264 encoding.
        if (tc.videoCodec == "h264" &&
            (tc.videoEncoder == "vaapi" || tc.videoEncoder == "qsv")) {
            const char* refsAb = std::getenv("DVBSTREAMER5_INTEL_H264_REFS_AB");
            if (refsAb && (std::strcmp(refsAb, "1") == 0 ||
                           std::strcmp(refsAb, "2") == 0)) {
                tc.refFrames = static_cast<std::uint32_t>(refsAb[0] - '0');
                std::cerr << "NATIVE INTEL H264 REFS AB stream="
                          << streamConfig.id << " refs=" << tc.refFrames
                          << " profile=" << tc.profile << std::endl;
            }
        }
        tc.openGop = autoTc.openGop;
        tc.aqStrength = autoTc.aqStrength;
        if (tc.videoCodec != "copy") {
            std::cerr << "NATIVE TRANSCODE AUTO stream=" << streamConfig.id
                      << " encoder=" << tc.videoEncoder
                      << " fps=" << tc.fps
                      << " configured_video_kbps=" << (requestedTranscodeVideoBitrate / 1000ULL)
                      << " effective_video_kbps=" << (tc.videoBitrate / 1000ULL)
                      << " preset=" << tc.preset
                      << " profile=" << tc.profile
                      << " level=" << tc.level
                      << " gop_frames=" << tc.gopFrames
                      << " aq_strength=" << tc.aqStrength << std::endl;
        }
        effectivePrimaryVideoBitrate = tc.videoBitrate;
        tc.serviceId = static_cast<std::uint16_t>(streamConfig.serviceId > 0 && streamConfig.serviceId <= 0xffff ? streamConfig.serviceId : 1);
        tc.videoPid = static_cast<std::uint16_t>(streamConfig.videoPid > 0 && streamConfig.videoPid <= 0x1ffe ? streamConfig.videoPid : 0x0100);
        tc.audioPid = static_cast<std::uint16_t>(streamConfig.audioPid > 0 && streamConfig.audioPid <= 0x1ffe ? streamConfig.audioPid : 0x0101);
        // V336: the final CBR transport clock and NULL stuffing are owned
        // exclusively by NativeUdpRelay's WISI/observed CBR pacer. The
        // transcoder mux must emit only PSI + actual A/V PES packets.
        // Previously a CBR tile generated media-clock NULL bursts here,
        // then the output shaper discarded and regenerated those NULLs.
        // On live HTTP/SRT inputs that extra work ran on the media pipeline
        // and could delay the next encoded frame across *all* output types.
        // A zero bitrate does NOT disable final UDP/SRT/HTTP CBR: relay below
        // still gets the saved targetBitrate and paceObservedTransport flag.
        tc.muxBitrate = 0;
        if (streamConfig.cbr && streamConfig.targetBitrate > 0) {
            std::cerr << "NATIVE TRANSCODER CBR SINGLE SHAPER"
                      << " stream_id=" << streamConfig.id
                      << " mux_null_stuffing=disabled"
                      << " output_cbr=enabled"
                      << " target_kbps="
                      << (dvbstreamer5::media::mpegts::CbrTsPacer::decodeTargetBitrate(
                              streamConfig.targetBitrate) / 1000ULL)
                      << " pcr_preroll_ms=500"
                      << std::endl;
        }
        tc.serviceName = streamConfig.serviceName.empty() ? streamConfig.name : streamConfig.serviceName;
        tc.serviceProvider = streamConfig.serviceProvider;
        // V10.8.102: keep configured display geometry for HLS ABR. Anamorphic
        // DVB SD can be coded as 720x576 while its intended display is 16:9;
        // treating coded pixels as square pixels used to collapse ABR to 4:3.
        // Scope this to multibitrate HLS so direct/single-bitrate behavior stays
        // exactly as before.
        if (hasHlsOutput && streamConfig.transcodeMultibitrateEnabled &&
            tc.videoCodec != "copy") {
            tc.lockOutputGeometry = true;
        }
        std::string transcodeError;
        if (!state->nativeTranscoder->initialize(tc, transcodeError)) {
            CardManager::instance().releaseService(streamConfig.id);
            if (error) *error = transcodeError.empty() ? "native transcoder initialization failed" : transcodeError;
            return false;
        }

        // V10.8.2: a multibitrate checkbox must create real independent
        // renditions, not merely point the UI at a non-existent master.m3u8.
        // Every lower HLS rendition receives the same post-remap/post-CA TS
        // for audio/demux state, while video decode/deinterlace is shared from
        // the primary pipeline. Each rendition owns only scale/encode/mux.
        if (hasHlsOutput && streamConfig.transcodeMultibitrateEnabled &&
            tc.videoCodec != "copy") {
            if (toLower(streamConfig.hlsContainer) != "mpegts") {
                CardManager::instance().releaseService(streamConfig.id);
                if (error) *error = "native multibitrate HLS currently requires MPEG-TS container";
                return false;
            }
            const auto profiles = makeAbrProfiles(tc.width, tc.height, effectivePrimaryVideoBitrate);
            for (const auto& profile : profiles) {
                auto variant = std::make_unique<StreamState::HlsAbrVariantRuntime>();
                variant->name = profile.name;
                variant->width = profile.width;
                variant->height = profile.height;
                variant->videoBitrate = profile.videoBitrate;
                variant->muxBitrate = profile.videoBitrate + tc.audioBitrate +
                    std::max<std::uint64_t>(300000ULL, profile.videoBitrate / 20ULL);
                variant->transcoder = std::make_unique<dvbstreamer5::media::transcode::NativeTranscoderPipeline>();
                if (configuredFailover) {
                    std::lock_guard<std::mutex> lock(managerMutex);
                    auto& saved = failoverHlsAbrOutputs[streamConfig.id][variant->name];
                    if (!saved)
                        saved = std::make_shared<
                            dvbstreamer5::media::hls::NativeHlsSegmenter>();
                    variant->segmenter = saved;
                } else {
                    variant->segmenter = std::make_shared<
                        dvbstreamer5::media::hls::NativeHlsSegmenter>();
                }
                auto abrTc = tc;
                abrTc.width = variant->width;
                abrTc.height = variant->height;
                abrTc.videoBitrate = variant->videoBitrate;
                abrTc.muxBitrate = variant->muxBitrate;
                const auto abrAuto = dvbstreamer5::app::automaticTranscodeSettings(
                    abrTc.fps, abrTc.videoBitrate,
                    static_cast<std::uint32_t>(abrTc.width),
                    static_cast<std::uint32_t>(abrTc.height), abrTc.videoEncoder);
                abrTc.preset = abrAuto.preset;
                abrTc.profile = abrAuto.profile;
                abrTc.level = abrAuto.level;
                abrTc.gopFrames = abrAuto.gopFrames;
                abrTc.bFrames = abrAuto.bFrames;
                abrTc.refFrames = abrAuto.refFrames;
                abrTc.openGop = abrAuto.openGop;
                abrTc.aqStrength = abrAuto.aqStrength;
                std::string abrError;
                if (!variant->transcoder->initialize(abrTc, abrError)) {
                    CardManager::instance().releaseService(streamConfig.id);
                    if (error) *error = "HLS ABR " + variant->name + " transcoder failed: " + abrError;
                    return false;
                }
                variant->transcoder->setExternalVideoInput(true);
                variant->transcoder->setExternalAudioInput(true);
                std::cerr << "NATIVE HLS ABR RENDITION init name=" << variant->name
                          << " size=" << variant->width << "x" << variant->height
                          << " video_kbps=" << (variant->videoBitrate / 1000ULL)
                          << " mux_kbps=" << (variant->muxBitrate / 1000ULL) << std::endl;
                state->hlsAbrVariants.push_back(std::move(variant));
            }
            if (!state->hlsAbrVariants.empty() && state->nativeTranscoder) {
                StreamState* statePtr = state.get();
                const StreamConfig abrMasterConfig = streamConfig;
                const std::uint64_t abrPrimaryVideoBitrate =
                    effectivePrimaryVideoBitrate;
                state->nativeTranscoder->setDecodedVideoObserver(
                    [statePtr, abrMasterConfig, abrPrimaryVideoBitrate](
                        std::shared_ptr<const dvbstreamer5::media::codec::RawVideoFrame> frame) {
                        if (!statePtr || !frame) return;

                        {
                            std::lock_guard<std::mutex> abrLock(statePtr->hlsAbrMutex);
                            if (!statePtr->hlsAbrSourceResolved) {
                                statePtr->hlsAbrSourceResolved = true;

                                auto primaryGeometry =
                                    statePtr->nativeTranscoder->configuredOutputGeometry();
                                // V10.8.102: keep the configured output geometry.
                                // Coded source dimensions do not describe display
                                // aspect for anamorphic DVB (for example 720x576
                                // carrying 16:9). Replacing the configured raster
                                // here made ABR advertise/output a 4:3-like stream.

                                statePtr->hlsAbrPrimaryWidth =
                                    primaryGeometry.first;
                                statePtr->hlsAbrPrimaryHeight =
                                    primaryGeometry.second;

                                const std::uint64_t primaryPixels =
                                    static_cast<std::uint64_t>(
                                        std::max(0, primaryGeometry.first)) *
                                    static_cast<std::uint64_t>(
                                        std::max(0, primaryGeometry.second));

                                std::size_t enabledVariants = 0;
                                for (auto& variant : statePtr->hlsAbrVariants) {
                                    if (!variant) continue;
                                    const std::uint64_t variantPixels =
                                        static_cast<std::uint64_t>(
                                            std::max(0, variant->width)) *
                                        static_cast<std::uint64_t>(
                                            std::max(0, variant->height));
                                    variant->enabled =
                                        variantPixels > 0 &&
                                        variantPixels < primaryPixels;
                                    if (variant->enabled) ++enabledVariants;
                                }

                                std::string masterError;
                                const std::uint64_t primaryTransportBitrate =
                                    abrMasterConfig.cbr &&
                                    abrMasterConfig.targetBitrate > 0
                                        ? abrMasterConfig.targetBitrate
                                        : abrPrimaryVideoBitrate +
                                            abrMasterConfig.transcodeAudioBitrate +
                                            180000ULL;
                                if (!writeAbrMasterPlaylist(
                                        abrMasterConfig,
                                        primaryGeometry.first,
                                        primaryGeometry.second,
                                        abrPrimaryVideoBitrate,
                                        primaryTransportBitrate,
                                        statePtr->hlsAbrVariants,
                                        masterError)) {
                                    std::cerr
                                        << "NATIVE HLS ABR runtime master error="
                                        << masterError << std::endl;
                                } else {
                                    std::cerr
                                        << "NATIVE HLS ABR SOURCE source="
                                        << frame->width << "x" << frame->height
                                        << " primary="
                                        << primaryGeometry.first << "x"
                                        << primaryGeometry.second
                                        << " lower_variants="
                                        << enabledVariants << std::endl;
                                }
                            }

                            for (auto& variant : statePtr->hlsAbrVariants) {
                                if (!variant || !variant->enabled ||
                                    !variant->transcoder)
                                    continue;
                                (void)variant->transcoder
                                    ->pushDecodedVideoFrame(frame);
                            }
                        }
                    });
                state->nativeTranscoder->setEncodedAudioObserver(
                    [statePtr](dvbstreamer5::media::mpegts::ElementaryCodec codec,
                               const dvbstreamer5::media::codec::EncodedAudioFrame& frame,
                               std::uint64_t duration90k) {
                        if (!statePtr) return;
                        std::lock_guard<std::mutex> abrLock(statePtr->hlsAbrMutex);
                        for (auto& variant : statePtr->hlsAbrVariants) {
                            if (!variant || !variant->enabled ||
                                !variant->transcoder)
                                continue;
                            (void)variant->transcoder
                                ->pushEncodedAudioFrame(codec, frame, duration90k);
                        }
                    });
                std::cerr << "NATIVE HLS ABR shared_decode=1 shared_audio=1 source_demuxers=1 renditions="
                          << (state->hlsAbrVariants.size() + 1) << std::endl;
            }
        }
    }
    const std::string normalizedInput = resolvedInputUri;
    const std::string inputMode = configuredInputMode;
    const bool hlsInput = inputMode == "hls" || toLower(normalizedInput).find(".m3u8") != std::string::npos ||
        toLower(normalizedInput).rfind("hls://", 0) == 0;
    const bool srtInput = toLower(normalizedInput).rfind("srt://", 0) == 0;
    const bool rtspInput = toLower(normalizedInput).rfind("rtsp://", 0) == 0;
    const bool rtmpInput = toLower(normalizedInput).rfind("rtmp://", 0) == 0 || toLower(normalizedInput).rfind("rtmps://", 0) == 0;
    const bool testInput = streamConfig.testPattern || inputMode == "test" ||
        toLower(normalizedInput) == "test://bars" || toLower(normalizedInput) == "testsrc://bars" ||
        toLower(normalizedInput) == "bars://hd";
    if (hlsInput) state->nativeHlsInput = std::make_shared<dvbstreamer5::media::hls::NativeHlsInput>();
    if (srtInput) state->nativeSrtInput = std::make_shared<dvbstreamer5::media::srt::NativeSrtInput>();
    if (rtspInput) state->nativeRtspInput = std::make_shared<dvbstreamer5::media::rtsp::NativeRtspInput>();
    if (rtmpInput) state->nativeRtmpInput = std::make_shared<dvbstreamer5::media::rtmp::NativeRtmpInput>();
    // V346: on automatic primary/backup transitions, reuse the existing SRT
    // outputs and their connected peers. stopStream parked them after draining
    // the old relay; no new port bind or client reconnect is required.
    bool reusedFailoverSrt = false;
    std::vector<std::unique_ptr<dvbstreamer5::media::srt::NativeSrtOutput>>
        obsoleteFailoverSrt;
    {
        std::lock_guard<std::mutex> lock(managerMutex);
        auto parked = parkedFailoverSrtOutputs.find(streamConfig.id);
        if (parked != parkedFailoverSrtOutputs.end()) {
            if (parked->second.size() == srtOutputSpecs.size() &&
                std::all_of(parked->second.begin(), parked->second.end(),
                    [](const auto& output) { return output && output->running(); })) {
                state->nativeSrtOutputs = std::move(parked->second);
                reusedFailoverSrt = true;
            } else {
                obsoleteFailoverSrt = std::move(parked->second);
            }
            parkedFailoverSrtOutputs.erase(parked);
        }
    }
    // Destroy any obsolete outputs outside managerMutex: their peer-state
    // callbacks acquire managerMutex via removeStreamSession().
    obsoleteFailoverSrt.clear();
    if (reusedFailoverSrt) {
        for (auto& output : state->nativeSrtOutputs)
            output->sourceSwitched(true);
        std::cerr << "NATIVE SRT FAILOVER PRESERVE"
                  << " stream_id=" << streamConfig.id
                  << " outputs=" << state->nativeSrtOutputs.size()
                  << " action=reuse_connected_sockets_and_rebase_ts"
                  << std::endl;
    }
    for (const auto& spec : srtOutputSpecs) {
        if (reusedFailoverSrt) break;
        auto output = std::make_unique<dvbstreamer5::media::srt::NativeSrtOutput>();
        dvbstreamer5::media::srt::EndpointConfig srtConfig;
        std::string host = spec.host.empty() ? std::string("0.0.0.0") : spec.host;
        std::string srtUri = "srt://" + host + ":" + std::to_string(spec.port) + "?mode=" +
            (spec.mode.empty() ? std::string("listener") : spec.mode);
        srtUri = dvbstreamer5::protocols::srt_vps::applyToUri(srtUri, streamConfig);
        std::string srtError;
        if (!dvbstreamer5::media::srt::parseUri(srtUri, srtConfig, srtError)) {
            CardManager::instance().releaseService(streamConfig.id);
            if (error) *error = srtError;
            return false;
        }
        srtConfig.latencyMs = std::clamp(spec.latency, 20, 60000);
        srtConfig.passphrase = spec.passphrase;
        srtConfig.streamId = spec.streamId;
        srtConfig.pbkeylen = spec.pbkeylen;
        srtConfig.bindAddress = cleanInterface(spec.iface);
        // Apply VPS/VDS tuning last so the normal per-output latency cannot
        // overwrite the optimization profile after URI parsing.
        srtConfig = dvbstreamer5::protocols::srt_vps::profile(srtConfig, streamConfig);
        srtConfig.diagnosticTileId = streamConfig.id;
        // Keep the listener acceptance check off the live SRT sender thread
        // even when backend mode is switched on without restarting a tile.
        srtConfig.asyncPeerAuthorization = srtConfig.mode=="listener";
        std::cerr << "SRT OUT route stream=" << streamConfig.id
                  << " mode=" << srtConfig.mode
                  << " local_bind=" << (srtConfig.bindAddress.empty() ? std::string("auto") : srtConfig.bindAddress)
                  << " host=" << srtConfig.host
                  << " port=" << srtConfig.port << std::endl;
        if (streamConfig.srtVpsVdsOptimization) {
            std::cerr << "SRT VPS/VDS effective OUT stream=" << streamConfig.id
                      << " mode=" << srtConfig.mode
                      << " latency_ms=" << srtConfig.latencyMs
                      << " rcvlatency_ms=" << srtConfig.receiveLatencyMs
                      << " peerlatency_ms=" << srtConfig.peerLatencyMs
                      << " rcvbuf=" << srtConfig.receiveBufferBytes
                      << " sndbuf=" << srtConfig.sendBufferBytes
                      << " fc=" << srtConfig.flightWindowPackets
                      << " payload=" << srtConfig.payloadSize
                      << " io_timeout_ms=" << srtConfig.ioTimeoutMs
                      << std::endl;
        }
        if (srtConfig.mode == "listener") srtConfig.host = "0.0.0.0";
        const std::string currentStreamId = streamConfig.id;
        if (!output->start(
                srtConfig,
                [this, currentStreamId, listener=srtConfig.mode=="listener"](
                    const std::string& peerIp,const std::string& peerSid){
                    const auto& conf=configManager.config;
                    if(listener && dvbstreamer5::stream_auth::enabled(conf,currentStreamId)){
                        return dvbstreamer5::stream_auth::SessionRegistry::instance().check(
                            conf,currentStreamId,peerIp,"srt","/srt/"+currentStreamId,
                            "SRT/1.5.7","",
                            dvbstreamer5::stream_auth::srtQuery(peerSid));
                    }
                    return isClientAllowedForStream(currentStreamId,peerIp);
                },
                [this, currentStreamId](const std::string& peerIp) {
                    if (!peerIp.empty()) addStreamSession(currentStreamId, peerIp, "srt");
                },
                [this, currentStreamId](const std::string& peerIp) {
                    if (!peerIp.empty()) removeStreamSession(currentStreamId, peerIp, "srt");
                },
                srtError)) {
            CardManager::instance().releaseService(streamConfig.id);
            if (error) *error = srtError.empty() ? "native SRT output failed" : srtError;
            return false;
        }
        // The shared relay now normalizes MPEG-TS before every output;
        // do not apply a second SRT-only clock transformation.
        state->nativeSrtOutputs.push_back(std::move(output));
    }
    for (const auto& spec : rtspOutputSpecs) {
        auto output = std::make_unique<dvbstreamer5::media::rtsp::NativeRtspOutput>();
        dvbstreamer5::media::rtsp::OutputConfig cfg; cfg.bindAddress=cleanInterface(spec.iface); if(cfg.bindAddress.empty())cfg.bindAddress=cleanInterface(spec.host); if(cfg.bindAddress.empty())cfg.bindAddress="0.0.0.0"; cfg.port=spec.port; cfg.streamName=streamConfig.id;
        std::string e; const std::string currentStreamId=streamConfig.id;
        if(!output->start(cfg,[this,currentStreamId](const std::string&ip){return isClientAllowedForStream(currentStreamId,ip);},
            [this,currentStreamId](const std::string&ip){if(!ip.empty())addStreamSession(currentStreamId,ip,"rtsp");},
            [this,currentStreamId](const std::string&ip){if(!ip.empty())removeStreamSession(currentStreamId,ip,"rtsp");},e)) {
            CardManager::instance().releaseService(streamConfig.id); if(error)*error=e.empty()?"native RTSP output failed":e; return false;
        }
        std::cerr << "RTSP OUT effective stream=" << streamConfig.id
                  << " local_bind=" << cfg.bindAddress
                  << " port=" << cfg.port << std::endl;
        state->nativeRtspOutputs.push_back(std::move(output));
    }
    for (const auto& spec : rtmpOutputSpecs) {
        auto output=std::make_unique<dvbstreamer5::media::rtmp::NativeRtmpOutput>(); dvbstreamer5::media::rtmp::EndpointConfig cfg; cfg.uri=spec.uri; cfg.bindAddress=cleanInterface(spec.iface);
        std::string e; if(!output->start(cfg,[statePtr=state.get()](const std::string&st){if(statePtr)statePtr->statusMessage="RTMP "+st;},e)) { CardManager::instance().releaseService(streamConfig.id); if(error)*error=e.empty()?"native RTMP output failed":e; return false; }
        std::cerr << "RTMP OUT effective stream=" << streamConfig.id
                  << " local_bind=" << (cfg.bindAddress.empty() ? std::string("auto") : cfg.bindAddress)
                  << std::endl;
        state->nativeRtmpOutputs.push_back(std::move(output));
    }
    if (hasHlsOutput) {
        if (toLower(streamConfig.hlsContainer) == "cmaf") {
            if (configuredFailover) {
                std::lock_guard<std::mutex> lock(managerMutex);
                auto& saved = failoverCmafOutputs[streamConfig.id];
                if (!saved)
                    saved = std::make_shared<
                        dvbstreamer5::media::cmaf::NativeCmafSegmenter>();
                state->nativeCmafSegmenter = saved;
            } else {
                state->nativeCmafSegmenter = std::make_shared<
                    dvbstreamer5::media::cmaf::NativeCmafSegmenter>();
            }
        } else {
            if (configuredFailover) {
                std::lock_guard<std::mutex> lock(managerMutex);
                auto& saved = failoverHlsOutputs[streamConfig.id];
                if (!saved)
                    saved = std::make_shared<
                        dvbstreamer5::media::hls::NativeHlsSegmenter>();
                state->nativeHlsSegmenter = saved;
            } else {
                state->nativeHlsSegmenter = std::make_shared<
                    dvbstreamer5::media::hls::NativeHlsSegmenter>();
            }
        }
    }

    dvbstreamer5::media::network::NativeUdpRelayConfig relay;
    relay.streamId = streamConfig.id;
    if (configuredFailover) {
        std::lock_guard<std::mutex> lock(managerMutex);
        auto& saved = failoverPrimaryPrograms[streamConfig.id];
        if (!saved)
            saved = std::make_shared<
                dvbstreamer5::media::mpegts::NativeFailoverProgramIdentity>();
        relay.primaryProgramIdentity = saved;
        relay.capturePrimaryProgram = !state->usingBackup;
    }
    if (configuredFailover) {
        std::lock_guard<std::mutex> lock(managerMutex);
        auto& clock = failoverOutputClocks[streamConfig.id];
        if (!clock)
            clock = std::make_shared<
                dvbstreamer5::media::srt::NativeSrtSwitchClock>();
        if (automaticSourceSwitch) {
            clock->sourceSwitched();
            std::cerr << "NATIVE OUTPUT TS SOURCE SWITCH"
                      << " stream_id=" << streamConfig.id
                      << " transitions=" << clock->transitions()
                      << " outputs=udp,http,srt,hls,cmaf"
                      << std::endl;
        }
        relay.outputSwitchClock = clock;
    }
    relay.inputUri = testInput ? "external://test" : (hlsInput ? "external://hls" : (srtInput ? "external://srt" : (rtspInput ? "external://rtsp" : (rtmpInput ? "external://rtmp" : normalizedInput))));
    relay.externallyFedInput = testInput || hlsInput || srtInput || rtspInput || rtmpInput;
    relay.httpTsInputJitterEnabled = streamConfig.httpTsInputJitterEnabled;
    relay.outputs = nativeOutputs;
    relay.allowNoNetworkOutput = (hasHttpOutput || hasHlsOutput || !srtOutputSpecs.empty() || !rtspOutputSpecs.empty() || !rtmpOutputSpecs.empty()) && nativeOutputs.empty();
    relay.inputInterfaceAddress = cleanInterface(streamConfig.inputInterfaceAddress);
    relay.inputInterfaceAddressConfigured = streamConfig.inputInterfaceAddressConfigured;
    relay.interfaceAddress = cleanInterface(streamConfig.interfaceAddress);
    relay.accessKeyMode = streamConfig.hlsAccessKeyMode;
    relay.accessKeyName = streamConfig.hlsAccessKeyName;
    relay.accessKeyValue = streamConfig.hlsAccessKeyValue;
    relay.userAgent = streamConfig.hlsUserAgent;
    relay.targetBitrate = streamConfig.targetBitrate;
    // V344: keep a replacement file repeating until the primary recovers.
    relay.fileInputLoop = state->usingBackup &&
        toLower(streamConfig.backupInputType) == "file";
    // V334: explicit per-tile setting, saved in the channel configuration.
    // No environmental stream selector is required. Guard the test against
    // all non-HTTP/SRT transports and transcoding. The physical CBR bitrate
    // can carry existing private startup-buffer encoding in its last 3 digits.
    const bool hasUdpCbrOutput = std::any_of(
        nativeOutputs.begin(), nativeOutputs.end(),
        [](const auto& output) { return output.outputType == "udp-cbr"; });
    const auto physicalTargetBitrate =
        dvbstreamer5::media::mpegts::CbrTsPacer::decodeTargetBitrate(
            streamConfig.targetBitrate);
    const bool ipSourceClockAb =
        dvbstreamer5::media::network::enableIpCbrSourceClock(
            streamConfig.ipSourcePcrPassthrough, toLower(normalizedInput),
            streamConfig.transcodeEnabled, streamConfig.cbr,
            hasUdpCbrOutput, physicalTargetBitrate >= 100000ULL &&
                physicalTargetBitrate % 1000ULL == 0ULL,
            testInput, hlsInput);
    // V517: preserve the user's 0/500/.../15000-ms startup-buffer
    // marker while selecting Source-PCR for the WISI channel. The old
    // +809 shortcut would silently skip output PCR smoothing for legacy
    // buffered CBR markers (for example, 8000705 => 3000 ms).
    relay.wisiSourcePcrStrict = wisiCbrProfileActive || ipSourceClockAb;
    // Existing opt-in for *transcoded* WISI profiles remains unchanged.
    const bool transcodePcrAb = streamConfig.transcodeEnabled &&
        streamConfig.cbr && std::getenv("DVBSTREAMER5_WISI_SOURCE_PCR_AB") &&
        std::string(std::getenv("DVBSTREAMER5_WISI_SOURCE_PCR_AB")) == "1";
    relay.wisiChameleonProfile = wisiCbrProfileActive;
    relay.vbrPeakBitsPerSecond = wisiVbrPeakBitsPerSecond;
    relay.wisiPreserveSourcePcr = wisiCbrProfileActive ||
        transcodePcrAb || ipSourceClockAb;
    relay.sourcePcrPacketPacing = dvbstreamer5::media::network::enableDvbSourcePcrPacketPacing(
        streamConfig.dvbSourcePcrPacing, DvbSatellite::isDvbUri(normalizedInput),
        streamConfig.transcodeEnabled, streamConfig.cbr, hasUdpCbrOutput,
        testInput, streamConfig.targetBitrate);
    if (ipSourceClockAb) {
        std::cerr << "NATIVE WISI SOURCE PCR AB"
                  << " stream_id=" << streamConfig.id
                  << " selected_by=stream_config"
                  << " input=" << (srtInput ? "srt" : "http")
                  << " video=passthrough"
                  << " pcr=preserve"
                  << " media_clock=source_pcr"
                  << " udp_cbr_bitrate_kbps=" << physicalTargetBitrate / 1000ULL
                  << " startup_buffer_ms="
                  << dvbstreamer5::media::mpegts::CbrTsPacer::startupBufferMilliseconds(
                         streamConfig.targetBitrate)
                  << " other_tiles=unchanged"
                  << std::endl;
    }
    // V341: only transcoded HLS with NO other configured output bypasses
    // network CBR pacing. The HLS segmenter receives the encoded MPEG-TS
    // directly; otherwise overflowing the network observer queue stops HLS.
    const bool transcodedHlsOnly =
        dvbstreamer5::media::hls::bypassNetworkCbrForTranscodedHlsOnly(
            streamConfig.transcodeEnabled, hasHlsOutput, hasHttpOutput,
            nativeOutputs.size(), srtOutputSpecs.size(),
            rtspOutputSpecs.size(), rtmpOutputSpecs.size());
    relay.paceObservedTransport =
        streamConfig.cbr && streamConfig.targetBitrate > 0 && !transcodedHlsOnly;
    relay.transcodedTransport =
        streamConfig.transcodeEnabled && relay.paceObservedTransport;
    if (transcodedHlsOnly && streamConfig.cbr) {
        std::cerr << "NATIVE HLS CBR OBSERVER BYPASS"
                  << " stream_id=" << streamConfig.id
                  << " reason=hls_only_no_network_cbr_output"
                  << " source_ts=post_transcode"
                  << std::endl;
    }

    if (streamConfig.epgEnabled && streamConfig.epgEitOutput) {
        // A worker-local generator probes the final PAT/SDT for actual output
        // SID/TSID/ONID and uses the same DVB-first/XMLTV fallback as /api/epg.
        // The callback is dormant until the per-stream toggle is enabled and
        // never touches encoder, descrambler or frontend ownership.
        auto generator = std::make_shared<DvbEitOutput>(
            [this, streamId = streamConfig.id] {
                return epgSnapshot(streamId);
            });
        relay.injectDvbEit = [generator](
            std::vector<dvbstreamer5::media::mpegts::Packet>& packets) {
            generator->process(packets);
        };
        std::cerr << "DVB EIT OUTPUT enabled stream=" << streamConfig.id
                  << " pid=0x12 source=selected-epg"
                  << " packetization=bounded after_transform=1"
                  << std::endl;
    }

    if (streamConfig.epgEnabled) {
        const auto epgServiceId = streamConfig.inputServiceId <= 0xffffU
            ? static_cast<std::uint16_t>(streamConfig.inputServiceId)
            : std::uint16_t{0};
        state->epgCollector =
            std::make_shared<dvbstreamer5::media::epg::DvbEpgCollector>(
                epgServiceId, streamConfig.epgDays);
        // Never reuse EIT events from a different source or service.
        state->epgCacheKey = streamConfig.id + "|" + streamConfig.inputUri +
            "|" + std::to_string(streamConfig.inputServiceId);
        (void)state->epgCollector->restoreFromDisk(state->epgCacheKey);
        std::cerr << "DVB EPG enabled stream=" << streamConfig.id
                  << " sid=" << epgServiceId
                  << " days=" << streamConfig.epgDays
                  << " source=eit-pid-0x12"
                  << std::endl;
    }

    DvbSatelliteParams dvbParams;
    std::string dvbError;
    bool sharedDvbInputSource = false;
    if (DvbSatellite::isDvbUri(streamConfig.inputUri)) {
        if (!DvbSatellite::parseUri(streamConfig.inputUri, dvbParams, dvbError)) {
            CardManager::instance().releaseService(streamConfig.id);
            if (error) *error = dvbError.empty() ? "invalid DVB input" : dvbError;
            return false;
        }

        // A physical DVB frontend/dvr tap belongs to the transponder, not to an
        // individual service.  Feed every service relay from one shared full-TS
        // reader so channels on the same transponder do not fight over dvr0.
        sharedDvbInputSource = true;
        relay.inputUri = "external://dvb-shared";
        relay.externallyFedInput = true;
        relay.dvbInputSource = false;
        // V10.8.105: keep paceObservedTransport enabled when CBR is selected.
        // Shared DVB only changes how input arrives; SRT/HTTP/HLS still require
        // the output CBR shaper to insert NULL packets up to targetBitrate.
        relay.dvbTuneConfig.adapter = dvbParams.adapter;
        relay.dvbTuneConfig.frontend = dvbParams.frontend;
        relay.dvbTuneConfig.frequencyKHz = dvbParams.frequencyKHz;
        relay.dvbTuneConfig.symbolRateK = dvbParams.symbolRateK;
        relay.dvbTuneConfig.bandwidthHz = dvbParams.bandwidthHz;
        relay.dvbTuneConfig.polarity = dvbParams.polarity;
        relay.dvbTuneConfig.deliverySystem = dvbParams.deliverySystem;
        relay.dvbTuneConfig.modulation = dvbParams.modulation;
        relay.dvbTuneConfig.fec = dvbParams.fec;
        relay.dvbTuneConfig.diseqcSource = dvbParams.diseqcSource;
        relay.dvbTuneConfig.lnbLof1KHz = dvbParams.lnbLof1KHz;
        relay.dvbTuneConfig.lnbLof2KHz = dvbParams.lnbLof2KHz;
        relay.dvbTuneConfig.lnbSlofKHz = dvbParams.lnbSlofKHz;
        relay.dvbTuneConfig.unicableMode = dvbParams.unicableMode;
        relay.dvbTuneConfig.unicableUserBand = dvbParams.unicableUserBand;
        relay.dvbTuneConfig.unicableFrequencyKHz = dvbParams.unicableFrequencyKHz;
        relay.dvbTuneConfig.unicablePosition = dvbParams.unicablePosition;
        relay.dvbTuneConfig.unicablePin = dvbParams.unicablePin;
        relay.dvbTuneConfig.streamId = dvbParams.streamId;
        relay.dvbTuneConfig.jitterMs = dvbParams.jitterMs;
        // The shared reader still owns one full-MPTS demux for the frontend.
        // V10.8.117 moves only per-service PAT/PMT filtering ahead of the relay
        // queue, so tuner ownership/tuning and the shared bus remain unchanged.
        relay.dvbTuneConfig.pids = "8192";
    }

    relay.remapEnabled = streamConfig.remapEnabled ||
        streamConfig.inputAudioPid != 0 ||
        (sharedDvbInputSource && streamConfig.inputServiceId > 0);
    if (relay.remapEnabled) {
        const uint32_t outSid = streamConfig.remapEnabled
            ? streamConfig.serviceId
            : (streamConfig.inputServiceId ? streamConfig.inputServiceId
                                           : streamConfig.serviceId);
        if (streamConfig.inputServiceId > 0xffff || outSid == 0 || outSid > 0xffff ||
            streamConfig.videoPid > 0xffff || streamConfig.audioPid > 0xffff) {
            CardManager::instance().releaseService(streamConfig.id);
            if (error) *error = "service/PID value is outside MPEG-TS range";
            return false;
        }
        relay.remapConfig.inputServiceId = static_cast<uint16_t>(streamConfig.inputServiceId);
        relay.remapConfig.outputServiceId = static_cast<uint16_t>(outSid);
        relay.remapConfig.outputVideoPid = streamConfig.remapEnabled ? static_cast<uint16_t>(streamConfig.videoPid) : 0;
        relay.remapConfig.outputAudioPid = streamConfig.remapEnabled ? static_cast<uint16_t>(streamConfig.audioPid) : 0;
        relay.remapConfig.inputAudioPid = static_cast<uint16_t>(streamConfig.inputAudioPid);
        relay.remapConfig.preserveSourceServiceId = !streamConfig.remapEnabled;
        relay.remapConfig.serviceName = streamConfig.serviceName.empty() ? streamConfig.name : streamConfig.serviceName;
        relay.remapConfig.serviceProvider = streamConfig.serviceProvider;
        // Live IP program composition can change upstream when another tile
        // switches to a replacement MP4. Do not stop a downstream SRT/UDP/
        // HTTP relay just because its previous selected A/V track disappears.
        // Leave satellite DVB service/track filtering STRICT.
        const std::string liveInput = toLower(configuredInputUri);
        const bool dynamicNetworkSource =
            startsWith(liveInput, "srt://") ||
            startsWith(liveInput, "udp://") ||
            startsWith(liveInput, "rtp://") ||
            startsWith(liveInput, "http://") ||
            startsWith(liveInput, "https://") ||
            startsWith(liveInput, "rtsp://") ||
            startsWith(liveInput, "rtmp://") ||
            startsWith(liveInput, "rtmps://") ||
            startsWith(liveInput, "hls://");
        if (dynamicNetworkSource)
            relay.remapConfig.allowMissingOutputMedia = true;
    }

    // Only backup source metadata is rewritten. Primary TS remains unchanged.
    // The backup's SID/PMT/media PIDs and SDT name may be unrelated to
    // the original tile; keep the original output identity for existing players.
    if (state->usingBackup && configuredFailover) {
        const auto primary = relay.primaryProgramIdentity
            ? relay.primaryProgramIdentity->snapshot()
            : dvbstreamer5::media::mpegts::NativeFailoverProgramSnapshot{};
        const auto validPid = [](std::uint32_t p) {
            return p >= 0x20U && p < 0x1fffU;
        };
        const std::uint32_t preferredSid = streamConfig.remapEnabled
            ? streamConfig.serviceId
            : (streamConfig.inputServiceId ? streamConfig.inputServiceId
                                           : streamConfig.serviceId);
        const std::uint16_t outputSid = primary.serviceId
            ? primary.serviceId
            : static_cast<std::uint16_t>(
                preferredSid >= 1U && preferredSid <= 0xffffU ? preferredSid : 1U);
        relay.remapEnabled = true;
        relay.remapConfig.inputServiceId = 0; // backup program auto-select
        relay.remapConfig.inputAudioPid = 0;   // backup audio PID independent
        relay.remapConfig.allowMissingOutputMedia = true;
        relay.remapConfig.outputServiceId = outputSid;
        relay.remapConfig.preserveSourceServiceId = false;
        relay.remapConfig.outputPmtPid = primary.hasProgram()
            ? primary.pmtPid : 0;
        relay.remapConfig.outputVideoPid = primary.videoPid
            ? primary.videoPid
            : (streamConfig.remapEnabled && validPid(streamConfig.videoPid)
               ? static_cast<std::uint16_t>(streamConfig.videoPid) : 0);
        relay.remapConfig.outputAudioPid = primary.audioPid
            ? primary.audioPid
            : (streamConfig.remapEnabled && validPid(streamConfig.audioPid)
               ? static_cast<std::uint16_t>(streamConfig.audioPid) : 0);
        relay.remapConfig.serviceName =
            streamConfig.serviceName.empty() ? streamConfig.name : streamConfig.serviceName;
        relay.remapConfig.serviceProvider = streamConfig.serviceProvider;
        std::cerr << "NATIVE BACKUP SERVICE IDENTITY"
                  << " stream_id=" << streamConfig.id
                  << " name=" << relay.remapConfig.serviceName
                  << " primary_sid=" << outputSid
                  << " primary_pmt_pid=" << relay.remapConfig.outputPmtPid
                  << " primary_video_pid=" << relay.remapConfig.outputVideoPid
                  << " primary_audio_pid=" << relay.remapConfig.outputAudioPid
                  << " input_sid=auto" << std::endl;
    }

    std::shared_ptr<SharedDvbServicePrefilter> sharedDvbPrefilter;
    if (sharedDvbInputSource && relay.remapEnabled) {
        sharedDvbPrefilter = std::make_shared<SharedDvbServicePrefilter>();
        // V10.8.239: An individual DVB service must never inherit the
        // transponder's NULL PID stuffing. In VBR mode that stuffing could
        // raise a ~2.7 Mbit/s service to tens of Mbit/s and overload the
        // downstream IPTV input. Keep the existing CBR shaper responsible
        // for adding its own NULL packets up to the configured target rate.
        // Only the shared-DVB selected-service path uses this prefilter;
        // full-MPTS paths and non-DVB inputs are not affected.
        const bool dropSourceNullPackets = true;
        std::string prefilterError;
        if (!sharedDvbPrefilter->initialize(
                streamConfig.id,
                relay.remapConfig,
                dropSourceNullPackets,
                prefilterError)) {
            CardManager::instance().releaseService(streamConfig.id);
            if (error) {
                *error = prefilterError.empty()
                    ? "shared DVB service prefilter initialization failed"
                    : prefilterError;
            }
            return false;
        }

        // The same proven Remapper now runs in the subscriber thread before
        // NativeUdpRelay's bounded external-input queue.  Do not remap twice.
        relay.remapEnabled = false;
        // V10.8.137: SharedDvbServicePrefilter emits a contiguous Packet[188]
        // array after selected-packet structural validation/remapping. The relay
        // may therefore use the trusted aligned framer fast path; a sync-grid
        // break still falls back to the legacy byte-resync/full-validation path.
        relay.trustedAlignedExternalInput = true;
        std::cerr << "SHARED DVB service prefilter enabled stream="
                  << streamConfig.id
                  << " sid=" << streamConfig.inputServiceId
                  << " drop_source_null=" << (dropSourceNullPackets ? 1 : 0)
                  << std::endl;
    }

    if (state->epgCollector && !sharedDvbInputSource) {
        auto epgCollector = state->epgCollector;
        relay.observeSourceTransport =
            [epgCollector](const std::uint8_t* data, std::size_t size) {
                epgCollector->push(data, size);
            };
    }

    if (!vdrCiPlusInput && !streamConfig.conditionalAccessClient.empty()) {
        const std::string streamId = streamConfig.id;
        relay.processTransport = [streamId](uint8_t* data, std::size_t size) {
            return CaBackendManager::instance().processTransport(streamId, data, size);
        };
    }
    if (!state->hlsAbrVariants.empty()) {
        StreamState* statePtr = state.get();
        relay.observeInputTransport = [statePtr](const std::uint8_t*, std::size_t) {
            if (!statePtr) return;
            // V10.8.12: ABR renditions no longer demux/decode the input TS
            // independently. Their video and audio are supplied by the primary
            // pipeline, so this callback only drains each rendition's mux output.
            //
            // V10.8.102: never hold hlsAbrMutex while pollOutput()/segmenter I/O
            // runs. The primary audio worker uses the same mutex for shared AAC
            // fan-out; holding it across playlist/segment writes delayed audio by
            // hundreds of milliseconds and drove the mux into its 500-ms A/V
            // envelope (late_audio growth + visible ABR stalls).
            for (auto& slot : statePtr->hlsAbrVariants) {
                StreamState::HlsAbrVariantRuntime* variant = nullptr;
                {
                    std::lock_guard<std::mutex> abrLock(statePtr->hlsAbrMutex);
                    if (!slot || !slot->enabled || slot->failed ||
                        !slot->transcoder || !slot->segmenter) {
                        continue;
                    }
                    variant = slot.get();
                }

                std::vector<std::uint8_t> encoded;
                std::string abrError;
                if (!variant->transcoder->pollOutput(encoded, abrError)) {
                    const std::string failure = abrError.empty()
                        ? "native ABR output poll failed" : abrError;
                    {
                        std::lock_guard<std::mutex> abrLock(statePtr->hlsAbrMutex);
                        variant->failed = true;
                        variant->lastError = failure;
                    }
                    std::cerr << "NATIVE HLS ABR ERROR name=" << variant->name
                              << " error=" << failure << std::endl;
                    continue;
                }
                if (!encoded.empty() && !variant->segmenter->push(encoded.data(), encoded.size())) {
                    const std::string failure = variant->segmenter->lastError();
                    {
                        std::lock_guard<std::mutex> abrLock(statePtr->hlsAbrMutex);
                        variant->failed = true;
                        variant->lastError = failure;
                    }
                    std::cerr << "NATIVE HLS ABR ERROR name=" << variant->name
                              << " error=" << failure << std::endl;
                }
            }
        };
    }
    {
        auto previousInputObserver = relay.observeInputTransport;
        auto previewHubForBrowser = state->nativePreviewHub;
        StreamState* previewState = state.get();
        auto previewSelector = std::make_shared<BrowserPreviewCodecSelector>();
        const auto previewTranscodeConfig = previewTc;
        relay.observeInputTransport =
            [previousInputObserver, previewHubForBrowser, previewState,
             previewSelector, previewTranscodeConfig](
                const std::uint8_t* data, std::size_t size) mutable {
                if (previousInputObserver) previousInputObserver(data, size);
                if (!previewHubForBrowser || !previewState ||
                    previewHubForBrowser->subscriberCount() == 0) {
                    return;
                }

                std::string previewProbeError;
                const auto previewMode =
                    previewSelector->inspect(data, size, previewProbeError);
                if (previewMode == BrowserPreviewCodecSelector::Mode::Detecting) {
                    return;
                }

                if (previewSelector->takeResolvedLog()) {
                    std::cerr << "NATIVE BROWSER PREVIEW route="
                              << (previewMode == BrowserPreviewCodecSelector::Mode::Passthrough
                                  ? "passthrough" : "transcode")
                              << " stream=" << previewState->config.name
                              << " input_video=" << previewSelector->videoCodecName()
                              << " input_audio=" << previewSelector->audioCodecName();
                    if (!previewProbeError.empty()) {
                        std::cerr << " probe_error=" << previewProbeError;
                    }
                    std::cerr << std::endl;
                }

                if (previewMode == BrowserPreviewCodecSelector::Mode::Passthrough) {
                    previewHubForBrowser->publish(data, size);
                    return;
                }

                if (previewState->previewTranscodeFailed.load(
                        std::memory_order_acquire)) {
                    return;
                }

                if (!previewState->nativePreviewTranscoder) {
                    auto transcoder = std::make_unique<
                        dvbstreamer5::media::transcode::NativeTranscoderPipeline>();
                    std::string previewInitError;
                    if (!transcoder->initialize(
                            previewTranscodeConfig, previewInitError)) {
                        previewState->previewTranscodeFailed.store(
                            true, std::memory_order_release);
                        std::cerr << "NATIVE BROWSER PREVIEW ERROR stream="
                                  << previewState->config.name
                                  << " error=" << (previewInitError.empty()
                                      ? "preview transcoder initialization failed"
                                      : previewInitError)
                                  << std::endl;
                        return;
                    }
                    previewState->nativePreviewTranscoder = std::move(transcoder);
                    std::cerr << "NATIVE BROWSER PREVIEW transcoder-start stream="
                              << previewState->config.name
                              << " codec=h264/aac size=1280x720"
                              << " video_kbps=1800 audio_kbps=128"
                              << " encoder=auto"
                              << std::endl;
                }

                std::vector<std::uint8_t> browserTs;
                std::string previewError;
                if (!previewState->nativePreviewTranscoder->process(
                        data, size, browserTs, previewError)) {
                    previewState->previewTranscodeFailed.store(
                        true, std::memory_order_release);
                    std::cerr << "NATIVE BROWSER PREVIEW ERROR stream="
                              << previewState->config.name
                              << " error=" << (previewError.empty()
                                  ? "H.264/AAC preview transcode failed" : previewError)
                              << std::endl;
                    return;
                }
                if (!browserTs.empty()) {
                    previewHubForBrowser->publish(
                        browserTs.data(), browserTs.size());
                }
            };
    }

    if (state->nativeTranscoder) {
        auto* transcoder = state->nativeTranscoder.get();
        relay.transformTransport = [transcoder](const std::uint8_t* data, std::size_t size,
                                                std::vector<std::uint8_t>& output, std::string& transformError) {
            return transcoder->process(data, size, output, transformError);
        };
    }

    {
        // Observe the selected service post-CA PAT/PMT for DVB, IPTV, HTTP,
        // SRT and HLS TS inputs. PMT inspection does not change transport.
        // Previously this monitor existed only for DVB subtitle inputs.
        auto subtitles = std::make_shared<DvbSubtitlePassthrough>(
            streamConfig.dvbSubtitlesLanguage, streamConfig.dvbSubtitlesTrack);
        state->dvbSubtitleMonitor = subtitles;
        if (DvbSatellite::isDvbUri(streamConfig.inputUri)) {
            // Preserve the existing DVB subtitle PES monitor.
            relay.observeDvbSubtitles = [subtitles](const std::uint8_t* data,
                                                    std::size_t size) {
                subtitles->observe(data, size);
            };
        } else {
            // IPTV/HLS/SRT/HTTP MPEG-TS only needs PAT/PMT diagnostics.
            // This cannot allocate/copy the complete 188-byte packet block.
            relay.observeMediaPmt = [subtitles](
                const std::vector<dvbstreamer5::media::mpegts::Packet>& packets) {
                subtitles->observeMediaPackets(packets);
            };
        }
        if (DvbSatellite::isDvbUri(streamConfig.inputUri) &&
            streamConfig.dvbSubtitlesEnabled) {
            // After transcoding we must reattach missing DVB PES and PMT.
            // With no transcoder the remapper already passes subtitle PES:
            // filter the original PMT/PIDs instead of duplicating packets.
            const bool transcoding = static_cast<bool>(state->nativeTranscoder);
            relay.injectDvbSubtitles = [subtitles, transcoding](
                std::vector<dvbstreamer5::media::mpegts::Packet>& packets) {
                if (transcoding) subtitles->inject(packets);
                else subtitles->filterPassthrough(packets);
            };
            std::cerr << "DVB SUBTITLES enabled stream=" << streamConfig.id
                      << " mode=" << (transcoding ? "post-transcode" : "native-pass")
                      << " language=" << streamConfig.dvbSubtitlesLanguage
                      << " track=" << streamConfig.dvbSubtitlesTrack
                      << std::endl;
        }
    }

    state->outputMediaMonitor = std::make_shared<DvbSubtitlePassthrough>();
    auto outputMediaMonitor = state->outputMediaMonitor;
    auto httpHub = state->nativeHttpHub;
    auto* hlsSegmenter = state->nativeHlsSegmenter.get();
    auto* cmafSegmenter = state->nativeCmafSegmenter.get();
    auto* mpts = mptsOutputManager.get();
    std::vector<dvbstreamer5::media::srt::NativeSrtOutput*> srtOutputs; for (auto& output : state->nativeSrtOutputs) srtOutputs.push_back(output.get());
    std::vector<dvbstreamer5::media::rtsp::NativeRtspOutput*> rtspOutputs; for (auto& output : state->nativeRtspOutputs) rtspOutputs.push_back(output.get());
    std::vector<dvbstreamer5::media::rtmp::NativeRtmpOutput*> rtmpOutputs; for (auto& output : state->nativeRtmpOutputs) rtmpOutputs.push_back(output.get());
    const std::string streamId = streamConfig.id;
    StreamState* outputStatsState = state.get();
    const auto hlsPushFailureReported = std::make_shared<std::atomic<bool>>(false);
    relay.observeTransport = [httpHub, hlsSegmenter, cmafSegmenter, mpts, srtOutputs, rtspOutputs, rtmpOutputs, streamId, outputStatsState, outputMediaMonitor, hlsPushFailureReported](const uint8_t* data, std::size_t size) {
        // Passive output PMT capture: the editor displays actual emitted codecs,
        // including native decoder fallback and audio copy, not a guess from settings.
        if (outputMediaMonitor) outputMediaMonitor->observe(data,size);
        // V10.8.70: dashboard decode state must describe the transport that
        // clients actually receive. Count payload/scrambling/PES only here,
        // after remap + CA descrambling + optional production transcoding.
        if (outputStatsState && data && size >= 188U) {
            std::uint64_t payloadPackets = 0;
            std::uint64_t scrambledPackets = 0;
            std::uint64_t clearPesStarts = 0;

            for (std::size_t offset = 0; offset + 188U <= size; offset += 188U) {
                const std::uint8_t* packet = data + offset;
                if (packet[0] != 0x47U) continue;

                const std::uint16_t pid = static_cast<std::uint16_t>(
                    (static_cast<std::uint16_t>(packet[1] & 0x1fU) << 8) | packet[2]);
                // Common PSI/SI and null packets do not describe A/V decode health.
                if (pid == 0x0000U || pid == 0x0001U ||
                    pid == 0x0011U || pid == 0x1fffU) {
                    continue;
                }

                const std::uint8_t adaptationControl =
                    static_cast<std::uint8_t>((packet[3] >> 4) & 0x03U);
                if ((adaptationControl & 0x01U) == 0) continue;

                std::size_t payloadOffset = 4U;
                if ((adaptationControl & 0x02U) != 0) {
                    payloadOffset = 5U + packet[4];
                    if (payloadOffset >= 188U) continue;
                }

                ++payloadPackets;

                const std::uint8_t scramblingControl =
                    static_cast<std::uint8_t>((packet[3] >> 6) & 0x03U);
                if (scramblingControl == 2U || scramblingControl == 3U) {
                    ++scrambledPackets;
                }

                if ((packet[1] & 0x40U) != 0 &&
                    scramblingControl == 0U &&
                    payloadOffset + 3U <= 188U &&
                    packet[payloadOffset] == 0x00U &&
                    packet[payloadOffset + 1U] == 0x00U &&
                    packet[payloadOffset + 2U] == 0x01U) {
                    ++clearPesStarts;
                }
            }

            outputStatsState->outputTsPayloadPackets.fetch_add(
                payloadPackets, std::memory_order_relaxed);
            outputStatsState->outputTsScrambledPackets.fetch_add(
                scrambledPackets, std::memory_order_relaxed);
            outputStatsState->outputTsClearPesStarts.fetch_add(
                clearPesStarts, std::memory_order_relaxed);
        }
        // V339: restore pre-V337 output ordering after OnDemand regression.
        // Keep the one-shot HLS failure diagnostic; it never stops the stream.
        if (hlsSegmenter && !hlsSegmenter->push(data, size) &&
            !hlsPushFailureReported->exchange(true, std::memory_order_relaxed)) {
            std::cerr << "NATIVE HLS OUTPUT FAILED"
                      << " stream_id=" << streamId
                      << " error=" << hlsSegmenter->lastError()
                      << std::endl;
        }
        if (cmafSegmenter) cmafSegmenter->push(data, size);
        for (auto* output : srtOutputs) if (output) output->push(data, size);
        for (auto* output : rtspOutputs) if (output) output->push(data, size);
        for (auto* output : rtmpOutputs) if (output) output->push(data, size);
        // HTTP MPEG-TS clients consume the finished production transport.
        if (httpHub) httpHub->publish(data, size);
        if (mpts) mpts->pushBytes(streamId, data, size);
    };

    if (state->nativeHlsSegmenter) {
        dvbstreamer5::media::hls::NativeHlsSegmenterConfig hlsConfig;
        hlsConfig.directory = streamConfig.hlsArchiveEnabled
            ? std::filesystem::path(streamConfig.hlsArchivePath) / streamConfig.id
            : std::filesystem::path("/tmp/dvbstreamer5-hls") / streamConfig.id;
        const auto segmentSettings = dvbstreamer5::media::hls::resolveSegmentSettings(
            streamConfig.hlsSegmentDuration, streamConfig.hlsSegmentCount, false);
        hlsConfig.durationExplicit = segmentSettings.customDuration;
        hlsConfig.countExplicit = segmentSettings.customCount;
        hlsConfig.targetDurationSeconds = segmentSettings.durationSeconds;
        hlsConfig.liveWindowSegments = segmentSettings.count;
        hlsConfig.archiveEnabled = streamConfig.hlsArchiveEnabled;
        hlsConfig.archiveHours = streamConfig.hlsArchiveHours;
        hlsConfig.independentSegments =
            streamConfig.transcodeEnabled &&
            toLower(streamConfig.transcodeVideoCodec) != "copy";
        hlsConfig.encryption = toLower(streamConfig.hlsEncryption);
        hlsConfig.keyUri = streamConfig.hlsEncryptionKeyUri;
        hlsConfig.hasKey = dvbstreamer5::media::hls::parseHexKey16(streamConfig.hlsEncryptionKeyHex, hlsConfig.key);
        std::string hlsError;
        const bool reuseHls = automaticSourceSwitch &&
            state->nativeHlsSegmenter->isRunning();
        if (!(reuseHls ? state->nativeHlsSegmenter->sourceSwitched()
                       : state->nativeHlsSegmenter->start(hlsConfig, hlsError))) {
            for (auto& output : state->nativeSrtOutputs) if (output) output->stop();
            CardManager::instance().releaseService(streamConfig.id);
            if (error) *error = hlsError.empty() ? "native HLS output failed" : hlsError;
            return false;
        }

        if (reuseHls) {
            std::cerr << "NATIVE HLS FAILOVER PRESERVE stream_id="
                      << streamConfig.id
                      << " container=mpegts action=retain_playlist_and_sequence"
                      << std::endl;
        }
        if (!state->hlsAbrVariants.empty()) {
            const auto baseDir = hlsConfig.directory;
            std::error_code abrEc;
            if (!streamConfig.hlsArchiveEnabled && !automaticSourceSwitch) {
                std::filesystem::remove_all(baseDir / "abr", abrEc);
                abrEc.clear();
                std::filesystem::remove(baseDir / "master.m3u8", abrEc);
                abrEc.clear();
            }
            for (auto& variant : state->hlsAbrVariants) {
                auto variantCfg = hlsConfig;
                variantCfg.directory = baseDir / "abr" / variant->name;
                std::string variantError;
                const bool reuseVariant = automaticSourceSwitch &&
                    variant->segmenter->isRunning();
                if (!(reuseVariant ? variant->segmenter->sourceSwitched()
                                   : variant->segmenter->start(variantCfg, variantError))) {
                    for (auto& started : state->hlsAbrVariants) {
                        if (started && started->segmenter) started->segmenter->stop();
                    }
                    state->nativeHlsSegmenter->stop();
                    CardManager::instance().releaseService(streamConfig.id);
                    if (error) *error = "HLS ABR " + variant->name + " segmenter failed: " + variantError;
                    return false;
                }
                std::cerr << "NATIVE HLS ABR SEGMENTER start name=" << variant->name
                          << " directory=" << variantCfg.directory.string() << std::endl;
            }
            int primaryW = 0, primaryH = 0;
            if (!TranscoderModule::resolutionSize(streamConfig.transcodeResolution, primaryW, primaryH)) {
                primaryW = 1920; primaryH = 1080;
            }
            std::string masterError;
            const std::uint64_t primaryTransportBitrate =
                streamConfig.cbr && streamConfig.targetBitrate > 0
                    ? streamConfig.targetBitrate
                    : effectivePrimaryVideoBitrate + streamConfig.transcodeAudioBitrate + 180000ULL;
            if (!writeAbrMasterPlaylist(streamConfig, primaryW, primaryH,
                                        effectivePrimaryVideoBitrate,
                                        primaryTransportBitrate,
                                        state->hlsAbrVariants, masterError)) {
                for (auto& started : state->hlsAbrVariants) {
                    if (started && started->segmenter) started->segmenter->stop();
                }
                state->nativeHlsSegmenter->stop();
                CardManager::instance().releaseService(streamConfig.id);
                if (error) *error = masterError;
                return false;
            }
            std::cerr << "NATIVE HLS ABR MASTER ready variants="
                      << (state->hlsAbrVariants.size() + 1)
                      << " primary_video_kbps=" << (effectivePrimaryVideoBitrate / 1000ULL)
                      << " primary_transport_kbps=" << (primaryTransportBitrate / 1000ULL)
                      << " path=" << (baseDir / "master.m3u8").string() << std::endl;
        }
    }
    if (state->nativeCmafSegmenter) {
        const std::string enc = toLower(streamConfig.hlsEncryption);
        if (enc != "none" && enc != "sample-aes") {
            CardManager::instance().releaseService(streamConfig.id);
            if (error) *error = "CMAF/fMP4 output supports none or SAMPLE-AES (cbcs); AES-128 whole-segment mode is MPEG-TS HLS only";
            return false;
        }
        dvbstreamer5::media::cmaf::SegmenterConfig cfg;
        cfg.directory = streamConfig.hlsArchiveEnabled
            ? std::filesystem::path(streamConfig.hlsArchivePath) / streamConfig.id
            : std::filesystem::path("/tmp/dvbstreamer5-hls") / streamConfig.id;
        const auto segmentSettings = dvbstreamer5::media::hls::resolveSegmentSettings(
            streamConfig.hlsSegmentDuration, streamConfig.hlsSegmentCount, true);
        cfg.targetDurationSeconds = segmentSettings.durationSeconds;
        cfg.liveWindowSegments = segmentSettings.count;
        cfg.archiveEnabled = streamConfig.hlsArchiveEnabled;
        cfg.archiveHours = streamConfig.hlsArchiveHours;
        cfg.encryption = enc;
        cfg.keyUri = streamConfig.hlsEncryptionKeyUri;
        cfg.hasKey = dvbstreamer5::media::hls::parseHexKey16(streamConfig.hlsEncryptionKeyHex, cfg.key);
        std::string e;
        const bool reuseCmaf = automaticSourceSwitch &&
            state->nativeCmafSegmenter->isRunning();
        if (!(reuseCmaf ? state->nativeCmafSegmenter->sourceSwitched()
                        : state->nativeCmafSegmenter->start(cfg, e))) {
            CardManager::instance().releaseService(streamConfig.id);
            if (error) *error = e.empty() ? "native CMAF output failed" : e;
            return false;
        }
    }

    std::string relayError;
    if (!state->nativeRelay->start(relay, relayError)) {
        for (auto& output : state->nativeSrtOutputs) if (output) output->stop();
        if (state->nativeHlsSegmenter) state->nativeHlsSegmenter->stop();
        if (state->nativeCmafSegmenter) state->nativeCmafSegmenter->stop();
        CardManager::instance().releaseService(streamConfig.id);
        if (error) *error = relayError.empty() ? "native relay failed" : relayError;
        return false;
    }

    if (sharedDvbInputSource) {
        auto* relayPtr = state->nativeRelay.get();
        std::string sharedDvbError;
        {
            // Serialize the first tune/open with DVB scan/signal probes.  Joining
            // an already-running transponder is cheap and does not retune it.
            auto guard = DvbSatellite::acquireFrontendTuneGuard(dvbParams);
            if (!sharedDvbInputs.subscribe(
                    streamConfig.id,
                    relay.dvbTuneConfig,
                    [relayPtr, sharedDvbPrefilter,
                     epgCollector = state->epgCollector](
                        const std::uint8_t* data, std::size_t size) {
                        if (epgCollector) epgCollector->push(data, size);
                        if (sharedDvbPrefilter) {
                            return sharedDvbPrefilter->push(relayPtr, data, size);
                        }
                        return relayPtr->pushInput(data, size);
                    },
                    [relayPtr](const std::string& finishError) {
                        relayPtr->finishInput(
                            finishError.empty()
                                ? "shared DVB source stopped"
                                : finishError);
                    },
                    sharedDvbError, false,
                    sharedDvbPrefilter ? sharedDvbPrefilter->pidAdmission()
                                      : nullptr)) {
                state->nativeRelay->stop();
                for (auto& output : state->nativeSrtOutputs) if (output) output->stop();
                if (state->nativeHlsSegmenter) state->nativeHlsSegmenter->stop();
                if (state->nativeCmafSegmenter) state->nativeCmafSegmenter->stop();
                CardManager::instance().releaseService(streamConfig.id);
                if (error) {
                    *error = sharedDvbError.empty()
                        ? "shared DVB input setup failed"
                        : sharedDvbError;
                }
                return false;
            }
        }
    }

    if (state->nativeHlsInput) {
        auto* relayPtr = state->nativeRelay.get();
        std::string hlsError;
        if (!state->nativeHlsInput->start(
                streamConfig,
                [relayPtr](const std::uint8_t* data, std::size_t size) { return relayPtr->pushInput(data, size); },
                [relayPtr](const std::string& finishError) { relayPtr->finishInput(finishError); },
                hlsError)) {
            state->nativeRelay->stop();
            for (auto& output : state->nativeSrtOutputs) if (output) output->stop();
            if (state->nativeHlsSegmenter) state->nativeHlsSegmenter->stop();
            CardManager::instance().releaseService(streamConfig.id);
            if (error) *error = hlsError.empty() ? "native HLS input failed" : hlsError;
            return false;
        }
    }

    if (state->nativeSrtInput) {
        auto* relayPtr = state->nativeRelay.get();
        auto* statePtr = state.get();
        dvbstreamer5::media::srt::EndpointConfig srtConfig;
        std::string srtError;
        if (!dvbstreamer5::media::srt::parseUri(normalizedInput, srtConfig, srtError)) {
            state->nativeRelay->stop();
            for (auto& output : state->nativeSrtOutputs) if (output) output->stop();
            if (state->nativeHlsSegmenter) state->nativeHlsSegmenter->stop();
            CardManager::instance().releaseService(streamConfig.id);
            if (error) *error = srtError;
            return false;
        }
        if (inputMode == "caller" || inputMode == "listener") srtConfig.mode = inputMode;
        srtConfig.latencyMs = streamConfig.srtInputLatencyMs;
        if (!streamConfig.srtInputPassphrase.empty()) srtConfig.passphrase = streamConfig.srtInputPassphrase;
        if (!streamConfig.srtInputStreamId.empty()) srtConfig.streamId = streamConfig.srtInputStreamId;
        srtConfig.pbkeylen = streamConfig.srtInputPbKeyLen;
        srtConfig.bindAddress = cleanInterface(streamConfig.inputInterfaceAddress);
        srtConfig = dvbstreamer5::protocols::srt_vps::profile(srtConfig, streamConfig);
        srtConfig.diagnosticTileId = streamConfig.id;
        if (streamConfig.srtVpsVdsOptimization) {
            std::cerr << "SRT VPS/VDS effective IN stream=" << streamConfig.id
                      << " mode=" << srtConfig.mode
                      << " latency_ms=" << srtConfig.latencyMs
                      << " rcvlatency_ms=" << srtConfig.receiveLatencyMs
                      << " peerlatency_ms=" << srtConfig.peerLatencyMs
                      << " rcvbuf=" << srtConfig.receiveBufferBytes
                      << " sndbuf=" << srtConfig.sendBufferBytes
                      << " fc=" << srtConfig.flightWindowPackets
                      << " payload=" << srtConfig.payloadSize
                      << " io_timeout_ms=" << srtConfig.ioTimeoutMs
                      << std::endl;
        }
        if (!state->nativeSrtInput->start(
                srtConfig,
                [relayPtr](const std::uint8_t* data, std::size_t size) { return relayPtr->pushInput(data, size); },
                [statePtr](const std::string& status) { if (statePtr) statePtr->statusMessage = "SRT " + status; },
                srtError)) {
            state->nativeRelay->stop();
            for (auto& output : state->nativeSrtOutputs) if (output) output->stop();
            if (state->nativeHlsSegmenter) state->nativeHlsSegmenter->stop();
            CardManager::instance().releaseService(streamConfig.id);
            if (error) *error = srtError.empty() ? "native SRT input failed" : srtError;
            return false;
        }
    }

    if (state->nativeRtspInput) {
        auto* relayPtr=state->nativeRelay.get(); auto* statePtr=state.get(); dvbstreamer5::media::rtsp::InputConfig cfg; cfg.uri=normalizedInput; cfg.mode=(inputMode.find("udp")!=std::string::npos?"udp":(inputMode.find("tcp")!=std::string::npos?"tcp":"auto")); cfg.bindAddress=cleanInterface(streamConfig.inputInterfaceAddress); std::string e;
        if(!state->nativeRtspInput->start(cfg,[relayPtr](const std::uint8_t*d,std::size_t n){return relayPtr->pushInput(d,n);},[statePtr](const std::string&st){if(statePtr)statePtr->statusMessage="RTSP "+st;},e)){stopStream(streamConfig.id); state->nativeRelay->stop(); CardManager::instance().releaseService(streamConfig.id); if(error)*error=e.empty()?"native RTSP input failed":e; return false;}
    }
    if (state->nativeRtmpInput) {
        auto* relayPtr=state->nativeRelay.get(); auto* statePtr=state.get(); dvbstreamer5::media::rtmp::EndpointConfig cfg; cfg.uri=normalizedInput; cfg.bindAddress=cleanInterface(streamConfig.inputInterfaceAddress); std::string e;
        if(!state->nativeRtmpInput->start(cfg,[relayPtr](const std::uint8_t*d,std::size_t n){return relayPtr->pushInput(d,n);},[statePtr](const std::string&st){if(statePtr)statePtr->statusMessage="RTMP "+st;},e)){state->nativeRelay->stop(); CardManager::instance().releaseService(streamConfig.id); if(error)*error=e.empty()?"native RTMP input failed":e; return false;}
    }

    if (testInput) {
        auto* relayPtr = state->nativeRelay.get();
        auto* statePtr = state.get();
        dvbstreamer5::media::testpattern::NativeTestPatternConfig testConfig;
        testConfig.serviceId = static_cast<std::uint16_t>(streamConfig.serviceId > 0 && streamConfig.serviceId <= 0xffff ? streamConfig.serviceId : 1);
        testConfig.videoPid = static_cast<std::uint16_t>(streamConfig.videoPid > 0 && streamConfig.videoPid <= 0x1ffe ? streamConfig.videoPid : 0x0100);
        testConfig.audioPid = static_cast<std::uint16_t>(streamConfig.audioPid > 0 && streamConfig.audioPid <= 0x1ffe ? streamConfig.audioPid : 0x0101);
        testConfig.muxBitrate = 0; // wall-clock source pacing; output CBR remains handled by the normal pipeline.
        testConfig.videoBitrate = 2500000;
        testConfig.audioBitrate = 128000;
        testConfig.serviceName = streamConfig.serviceName.empty() ? streamConfig.name : streamConfig.serviceName;
        testConfig.serviceProvider = streamConfig.serviceProvider;
        state->testPatternStop.store(false);
        try {
            state->testPatternThread = std::thread([relayPtr, statePtr, testConfig]() mutable {
                dvbstreamer5::media::testpattern::NativeTestPatternSource source;
                std::string e;
                if (!source.run(testConfig,
                                [relayPtr](const std::uint8_t* d, std::size_t n) { return relayPtr->pushInput(d, n); },
                                statePtr->testPatternStop, e)) {
                    if (!statePtr->testPatternStop.load()) {
                        // If the relay stopped accepting input because the
                        // transcoder/output path failed, preserve the actual
                        // downstream error instead of hiding it behind the
                        // generic test-pattern sink message.
                        const std::string relayError = relayPtr->lastError();
                        statePtr->statusMessage = !relayError.empty()
                            ? relayError
                            : (e.empty() ? "native test-pattern generator failed" : e);
                        relayPtr->finishInput(statePtr->statusMessage);
                    }
                }
            });
        } catch (const std::exception& ex) {
            state->nativeRelay->stop();
            CardManager::instance().releaseService(streamConfig.id);
            if (error) *error = std::string("native test-pattern thread failed: ") + ex.what();
            return false;
        }
    }

    state->startedAt = std::chrono::steady_clock::now();
    state->active.store(true);
    state->running.store(true);
    state->statusMessage = testInput ? "running (native test pattern)" : (hlsInput ? "running (native HLS input)" : (srtInput ? "running (native SRT input)" : (rtspInput ? "running (native RTSP input)" : (rtmpInput ? "running (native RTMP input)" : "running (native media engine)"))));
    if (streamConfig.transcodeEnabled) state->statusMessage += " + native transcoder";
    StreamState* rawState = state.get();
    {
        std::lock_guard<std::mutex> lock(managerMutex);
        streams.emplace(streamConfig.id, std::move(state));
    }
    CardManager::instance().activateService(streamConfig.id);
    try {
        rawState->monitorThread = std::thread(&StreamManager::monitorNativeStream, this, rawState);
    } catch (const std::exception& ex) {
        stopStream(streamConfig.id);
        if (error) *error = std::string("native monitor thread failed: ") + ex.what();
        return false;
    }
    // Only the stream that explicitly configured a replacement source is
    // monitored for failover. Never auto-enable on unrelated live channels.
    if (!streamConfig.backupInputUri.empty() &&
        streamConfig.backupInputUri != streamConfig.inputUri) {
        std::lock_guard<std::mutex> lock(failoverMutex);
        if (!failoverEntries.count(streamConfig.id)) {
            FailoverEntry entry;
            entry.primary = streamConfig;
            entry.firstStartedAt = std::chrono::steady_clock::now();
            failoverEntries.emplace(streamConfig.id, std::move(entry));
        }
    }
    sendTelegramStreamState(
        telegramNotifier, configManager, streamConfig, "🟢",
        telegramText(configManager, "Поток запущен", "Stream started"),
        telegramText(configManager, "Native media engine работает", "Native media engine is running"));
    return true;
}

void StreamManager::configureAutoStartRecovery() {
    std::map<std::string, StreamConfig> configured;
    for (const auto& cfg : configManager.config.streams) {
        if (dvbstreamer5::stream_recovery::eligible(
                cfg.autoStart, toLower(cfg.activationMode))) {
            configured.emplace(cfg.id, cfg);
        }
    }
    std::lock_guard<std::mutex> lock(autoRecoveryMutex_);
    for (auto it = autoRecoveryAttempts_.begin(); it != autoRecoveryAttempts_.end();) {
        const auto next = configured.find(it->first);
        const auto previous = autoRecoveryConfigs_.find(it->first);
        if (next == configured.end() || previous == autoRecoveryConfigs_.end() ||
            previous->second.toJson() != next->second.toJson())
            it = autoRecoveryAttempts_.erase(it);
        else
            ++it;
    }
    autoRecoveryConfigs_.swap(configured);
}

void StreamManager::forgetAutoStartRecovery(const std::string& id) {
    std::lock_guard<std::mutex> lock(autoRecoveryMutex_);
    autoRecoveryConfigs_.erase(id);
    autoRecoveryAttempts_.erase(id);
}

void StreamManager::monitorAutoStartRecovery(unsigned workerIndex) {
    const auto waitFor = [this](unsigned seconds) {
        for (unsigned i = 0; i < seconds * 4U; ++i) {
            if (autoRecoveryStop_.load(std::memory_order_acquire)) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        return !autoRecoveryStop_.load(std::memory_order_acquire);
    };
    // Give the normal parallel startup workers time to initialize DVB/CA/codecs.
    if (!waitFor(30)) return;
    while (waitFor(5)) {
        const auto now = std::chrono::steady_clock::now();
        std::vector<StreamConfig> candidates;
        {
            std::lock_guard<std::mutex> lock(autoRecoveryMutex_);
            for (const auto& [id, cfg] : autoRecoveryConfigs_) {
                if (std::hash<std::string>{}(id) % 3U != workerIndex) continue;
                const auto attempt = autoRecoveryAttempts_.find(id);
                if (attempt != autoRecoveryAttempts_.end() &&
                    now < attempt->second.retryAt) continue;
                candidates.push_back(cfg);
            }
        }
        for (const auto& candidate : candidates) {
            if (autoRecoveryStop_.load(std::memory_order_acquire)) return;
            const std::string& id = candidate.id;
            if (isOperatorStopped(id) || isStopPending(id)) continue;
            bool active = false;
            bool stable = false;
            std::string previousFailure;
            {
                std::lock_guard<std::mutex> lock(managerMutex);
                const auto it = streams.find(id);
                if (it != streams.end() && it->second) {
                    const auto& state = *it->second;
                    active = state.active.load(std::memory_order_acquire);
                    stable = active && state.running.load(std::memory_order_acquire) &&
                        now - state.startedAt >= std::chrono::minutes(2);
                    if (!active && state.nativeRelay)
                        previousFailure = state.nativeRelay->lastError();
                }
            }
            if (active) {
                if (stable) {
                    std::lock_guard<std::mutex> lock(autoRecoveryMutex_);
                    autoRecoveryAttempts_.erase(id);
                }
                continue;
            }
            // Existing source failover already owns its primary/backup state.
            {
                std::lock_guard<std::mutex> lock(failoverMutex);
                if (failoverEntries.count(id)) continue;
            }
            // Do not wait behind an operator stop or an ongoing failover.
            std::unique_lock<std::mutex> actionLock(failoverActionMutex, std::try_to_lock);
            if (!actionLock.owns_lock()) continue;
            if (isOperatorStopped(id) || isStopPending(id) || isStreamActive(id))
                continue;
            {
                std::lock_guard<std::mutex> lock(failoverMutex);
                if (failoverEntries.count(id)) continue;
            }
            StreamConfig cfg;
            unsigned attemptNumber = 0;
            {
                std::lock_guard<std::mutex> lock(autoRecoveryMutex_);
                const auto desired = autoRecoveryConfigs_.find(id);
                if (desired == autoRecoveryConfigs_.end()) continue;
                auto& attempt = autoRecoveryAttempts_[id];
                const auto checkNow = std::chrono::steady_clock::now();
                if (checkNow < attempt.retryAt) continue;
                cfg = desired->second;
                attemptNumber = ++attempt.attempts;
                attempt.retryAt = checkNow +
                    dvbstreamer5::stream_recovery::retryDelay(attemptNumber);
            }
            std::cerr << "AUTO START RECOVERY BEGIN stream_id=" << id
                      << " attempt=" << attemptNumber
                      << " previous_error=" << (previousFailure.empty() ? "unknown" : previousFailure)
                      << std::endl;
            std::string error;
            bool started = false;
            try {
                started = startStream(cfg, &error);
                if (!started && isStreamActive(id)) started = true;
            } catch (const std::exception& ex) {
                error = ex.what();
            } catch (...) {
                error = "unknown exception";
            }
            std::cerr << "AUTO START RECOVERY RESULT stream_id=" << id
                      << " attempt=" << attemptNumber
                      << " result=" << (started ? "running" : "failed")
                      << " retry_delay_s=" <<
                          dvbstreamer5::stream_recovery::retryDelay(attemptNumber).count()
                      << (error.empty() ? "" : " error=" + error)
                      << std::endl;
        }
    }
}

void StreamManager::monitorNativeStream(StreamState* state) {
    uint64_t lastIn = 0, lastOut = 0, lastPayloadOut = 0, lastCc = 0;
    // Existing SRT peers and their lifetime sent-byte counters survive
    // automatic failover. Never count historical bytes as a new 1-s bitrate.
    for (const auto& output : state->nativeSrtOutputs)
        if (output) lastOut += output->sentBytes();
    for (const auto& output : state->nativeRtspOutputs)
        if (output) lastOut += output->sentBytes();
    for (const auto& output : state->nativeRtmpOutputs)
        if (output) lastOut += output->sentBytes();
    // Use actual selected, non-NULL TS progress for failover, not merely
    // transport socket byte reception (which may contain NULL or bad RTP).
    uint64_t lastUsefulInput = 0;
    uint64_t lastTsPayloadPackets = 0;
    uint64_t lastTsScrambledPackets = 0;
    uint64_t lastTsClearPesStarts = 0;
    const bool selectedDvbService =
        DvbSatellite::isDvbUri(state->config.inputUri) &&
        state->config.inputServiceId > 0;
    std::array<std::uint64_t, 5> inputRateWindow{};
    std::array<std::uint64_t, 5> payloadRateWindow{};
    std::size_t rateWindowIndex = 0;
    std::size_t rateWindowSamples = 0;
    std::uint64_t monitorTicks = 0;
    // V519: differentiate network ingress progress from selected TS/CA
    // consumption; short socket bursts must not trigger spurious failover.
    const std::string inputUriLower = toLower(normalizeInputUri(state->config.inputUri));
    const bool bufferedNetworkInput =
        dvbstreamer5::media::network::InputLivenessPolicy::isNetworkSource(
            inputUriLower, toLower(state->config.inputMode));
    dvbstreamer5::media::network::InputLivenessPolicy inputLiveness(
        bufferedNetworkInput);
    bool telegramInputUnavailable = false;
    while (!state->monitorStop.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (state->monitorStop.load()) break;
        auto* relay = state->nativeRelay.get();
        if (!relay) break;
        // A shared DVB relay is fed with the complete multiplex. For a
        // configured service the tile must show that service's post-remap,
        // non-null bitrate rather than the 40-80 Mbit/s transponder rate.
        const uint64_t in = selectedDvbService
            ? relay->selectedInputBytes()
            : relay->sourceInputBytes();
        const uint64_t payloadOut = relay->payloadOutputBytes();
        uint64_t out = relay->outputBytes();
        for (const auto& output : state->nativeSrtOutputs) if (output) out += output->sentBytes();
        for (const auto& output : state->nativeRtspOutputs) if (output) out += output->sentBytes();
        for (const auto& output : state->nativeRtmpOutputs) if (output) out += output->sentBytes();
        const uint64_t cc = relay->continuityErrors();
        state->inputBytes.store(in);
        state->outputBytes.store(out);
        const std::uint64_t inputDelta = in - lastIn;
        const std::uint64_t payloadDelta = payloadOut - lastPayloadOut;
        inputRateWindow[rateWindowIndex] = inputDelta;
        payloadRateWindow[rateWindowIndex] = payloadDelta;
        rateWindowIndex = (rateWindowIndex + 1U) % inputRateWindow.size();
        rateWindowSamples = (std::min)(rateWindowSamples + 1U, inputRateWindow.size());
        std::uint64_t inputWindowBytes = 0;
        std::uint64_t payloadWindowBytes = 0;
        for (std::size_t i = 0; i < rateWindowSamples; ++i) {
            inputWindowBytes += inputRateWindow[i];
            payloadWindowBytes += payloadRateWindow[i];
        }
        const uint64_t inputRate = (inputWindowBytes * 8U) / rateWindowSamples;
        const uint64_t payloadRate = (payloadWindowBytes * 8U) / rateWindowSamples;
        state->inputBitrate.store(inputRate);
        // Connection counters may reset on reconnect. Do not let unsigned
        // subtraction turn a counter reset into a fake multi-Gbit/s bitrate.
        const uint64_t outRate = out >= lastOut ? (out - lastOut) * 8U : 0U;
        // V10.8.106: CBR for HTTP/SRT is produced by the observer pacer and is
        // independent of whether an HTTP client or SRT peer is currently
        // connected.  sentBytes therefore is not a valid stream-level CBR
        // meter.  Once real channel payload has started, show the configured
        // shaped transport rate in the tile; VBR keeps the measured rate.
        const bool cbrTransportStarted =
            state->config.cbr && state->config.targetBitrate > 0 && payloadOut > 0;
        state->outputBitrate.store(
            cbrTransportStarted
                ? state->config.targetBitrate
                : (outRate ? outRate : payloadRate));
        state->outputPayloadBitrate.store(payloadRate);
        ++monitorTicks;
        if (state->config.transcodeEnabled && (monitorTicks % 5U) == 0U) {
            std::cerr << "NATIVE RATE stream=" << state->config.name
                      << " source_kbps=" << (state->inputBitrate.load() / 1000)
                      << " payload_kbps=" << (state->outputPayloadBitrate.load() / 1000)
                      << " cbr_kbps=" << (state->outputBitrate.load() / 1000)
                      << " cc_delta=" << (cc - lastCc)
                      << std::endl;
        }
        // Diagnose transport-only SRT freezes separately from AV encoding
        // stalls. The old droppedBytes counter included harmless offline
        // listener bytes and could show hundreds of MiB of fake queue loss.
        if (!state->nativeSrtOutputs.empty() && (monitorTicks % 10U) == 0U) {
            for (std::size_t index = 0; index < state->nativeSrtOutputs.size();
                 ++index) {
                const auto& output = state->nativeSrtOutputs[index];
                if (!output) continue;
                std::cerr << "NATIVE SRT OUTPUT HEALTH"
                          << " stream_id=" << state->config.id
                          << " output_index=" << index
                          << " connected=" << (output->connected() ? 1 : 0)
                          << " queued_bytes=" << output->queuedBytes()
                          << " sent_bytes=" << output->sentBytes()
                          << " queue_overrun_bytes=" << output->queueOverrunBytes()
                          << " no_peer_discard_bytes=" << output->noPeerDiscardBytes()
                          << " total_dropped_bytes=" << output->droppedBytes()
                          << " reconnects=" << output->reconnects()
                          << std::endl;
            }
        }

        // Runtime input-health notification must use packet progress, not the
        // absolute cumulative byte counter.  A UDP/SRT/HTTP relay can stay alive
        // forever after packets disappear, so `in == 0` only detects the special
        // case where the stream never delivered a byte.  Five seconds without
        // counter progress is treated as an outage; an explicit relay error is
        // reported immediately.  Any later byte progress is a recovery.
        const std::string relayErrorNow = relay->lastError();
        const uint64_t usefulInput = relay->selectedInputBytes();
        const auto inputHealth = inputLiveness.observe(
            usefulInput, relay->sourceInputBytes(),
            std::chrono::steady_clock::now());
        const bool inputAdvanced = inputHealth.mediaAdvanced;
        const bool recoveredFromInputLoss =
            inputAdvanced && telegramInputUnavailable;
        state->inputStalled.store(inputHealth.stalled, std::memory_order_release);
        if (bufferedNetworkInput && inputHealth.jitterGrace &&
            (monitorTicks % 5U) == 0U) {
            std::cerr << "NATIVE INPUT JITTER GRACE"
                      << " stream_id=" << state->config.id
                      << " media_silence_ms=" << inputHealth.mediaSilenceMs
                      << " network_silence_ms=" << inputHealth.sourceSilenceMs
                      << " incoming_bytes_advanced=" << (inputHealth.sourceAdvanced ? 1 : 0)
                      << " action=keep_live"
                      << std::endl;
        }
        if (inputAdvanced) {
            if (recoveredFromInputLoss) {
                sendTelegramStreamState(
                    telegramNotifier, configManager, state->config, "🟢",
                    telegramText(configManager, "Входной поток восстановлен", "Input stream recovered"),
                    telegramText(configManager, "Медиаданные снова поступают", "Media data is flowing again"));
                telegramInputUnavailable = false;
            }

            if (lastIn == 0 || recoveredFromInputLoss) {
                const std::string normalized = normalizeInputUri(state->config.inputUri);
                const std::string mode = toLower(state->config.inputMode);
                const bool vdrCiPlus =
                    dvbstreamer5::stream_protocols::inputs::isVdrInput(
                        toLower(normalized), mode, state->config.testPattern);
                const bool hls = mode == "hls" || toLower(normalized).find(".m3u8") != std::string::npos ||
                    toLower(normalized).rfind("hls://", 0) == 0;
                const bool srt = toLower(normalized).rfind("srt://", 0) == 0;
                const bool rtsp = toLower(normalized).rfind("rtsp://", 0) == 0;
                const bool rtmp = toLower(normalized).rfind("rtmp://", 0) == 0 || toLower(normalized).rfind("rtmps://", 0) == 0;
                state->statusMessage = vdrCiPlus ? "running (VDR CI+ input)" :
                    (hls ? "running (native HLS input)" :
                    (srt ? "running (native SRT input)" :
                    (rtsp ? "running (native RTSP input)" :
                    (rtmp ? "running (native RTMP input)" : "running (native media engine)"))));
                if (state->config.transcodeEnabled) state->statusMessage += " + native transcoder";
            }
        } else {
            const bool inputTimedOut = inputHealth.stalled;
            if (!relayErrorNow.empty() || inputTimedOut) {
                const std::string inputLossDetails = !relayErrorNow.empty()
                    ? relayErrorNow
                    : telegramText(
                        configManager,
                        "Нет входных медиаданных (ожидание при неравномерном приёме исчерпано)",
                        "Input media stalled (bounded network jitter grace exhausted)");
                if (inputTimedOut && !telegramInputUnavailable) {
                    std::cerr << "NATIVE INPUT HEALTH LOST"
                              << " stream_id=" << state->config.id
                              << " cause=" << inputHealth.cause
                              << " media_silence_ms=" << inputHealth.mediaSilenceMs
                              << " network_silence_ms=" << inputHealth.sourceSilenceMs
                              << " network_input=" << (bufferedNetworkInput ? 1 : 0)
                              << std::endl;
                }
                state->statusMessage = inputLossDetails;
                if (!telegramInputUnavailable) {
                    sendTelegramStreamState(
                        telegramNotifier, configManager, state->config, "🔴",
                        telegramText(configManager, "Входной поток недоступен", "Input stream unavailable"),
                        inputLossDetails);
                    telegramInputUnavailable = true;
                }
            }
        }
        state->inputCcErrors.store(cc);
        state->inputCcErrorsDelta.store(cc - lastCc);

        const uint64_t tsPayloadPackets =
            state->outputTsPayloadPackets.load(std::memory_order_relaxed);
        const uint64_t tsScrambledPackets =
            state->outputTsScrambledPackets.load(std::memory_order_relaxed);
        const uint64_t tsClearPesStarts =
            state->outputTsClearPesStarts.load(std::memory_order_relaxed);
        state->outputTsPayloadPacketsDelta.store(
            tsPayloadPackets - lastTsPayloadPackets, std::memory_order_relaxed);
        state->outputTsScrambledPacketsDelta.store(
            tsScrambledPackets - lastTsScrambledPackets, std::memory_order_relaxed);
        state->outputTsClearPesStartsDelta.store(
            tsClearPesStarts - lastTsClearPesStarts, std::memory_order_relaxed);
        lastTsPayloadPackets = tsPayloadPackets;
        lastTsScrambledPackets = tsScrambledPackets;
        lastTsClearPesStarts = tsClearPesStarts;

        lastIn = in;
        lastOut = out;
        lastPayloadOut = payloadOut;
        lastCc = cc;
        if (!relay->isRunning()) {
            // V349: a crashed input with a configured backup must not write
            // EXT-X-ENDLIST before the asynchronous failover worker recovers.
            const bool canFailover =
                !state->config.backupInputUri.empty() &&
                state->config.backupInputUri != state->config.inputUri;
            if (!canFailover) {
                if (state->nativeHlsSegmenter) state->nativeHlsSegmenter->stop();
                for (auto& variant : state->hlsAbrVariants) {
                    if (variant && variant->segmenter) variant->segmenter->stop();
                }
            }
            state->running.store(false);
            state->active.store(false);
            const std::string relayError = relay->lastError();
            state->statusMessage = relayError.empty() ? "native input stopped" : relayError;
            // V339: a failed output observer or encoder can otherwise be
            // mistaken for an HTTP-client disconnect. Record the root relay
            // failure alongside the tile ID before any OnDemand teardown.
            std::cerr << "NATIVE STREAM RELAY STOP"
                      << " stream_id=" << state->config.id
                      << " transcode=" << (state->config.transcodeEnabled ? 1 : 0)
                      << " cbr=" << (state->config.cbr ? 1 : 0)
                      << " reason=" << state->statusMessage
                      << std::endl;
            sendTelegramStreamState(
                telegramNotifier, configManager, state->config, "🔴",
                telegramText(configManager, "Поток аварийно остановлен", "Stream stopped unexpectedly"),
                state->statusMessage);
            break;
        }
    }
}

bool StreamManager::restartStream(const StreamConfig& cfg, std::string* error) {
    std::lock_guard<std::mutex> actionLock(failoverActionMutex);
    // Explicit UI/scheduled restart supersedes any automatic failover plan.
    {
        std::lock_guard<std::mutex> lock(failoverMutex);
        failoverEntries.erase(cfg.id);
    }
    stopStream(cfg.id);
    return startStream(cfg, error);
}

bool StreamManager::stopStream(const std::string& id) {
    bool automaticFailoverTransition = false;
    {
        std::lock_guard<std::mutex> lock(failoverMutex);
        const auto it = failoverEntries.find(id);
        automaticFailoverTransition =
            it != failoverEntries.end() && it->second.switching;
        if (it != failoverEntries.end() && !it->second.switching)
            failoverEntries.erase(it);
    }
    std::unique_ptr<StreamState> state;
    {
        std::lock_guard<std::mutex> lock(managerMutex);
        auto it = streams.find(id);
        if (it == streams.end()) return false;
        state = std::move(it->second);
        streams.erase(it);
        for (const auto& [fd, session] : httpClients) {
            if (session.streamId == id) {
                // Keep public HTTP TS playback on the original TCP socket.
                // Browser preview belongs to the old input generation.
                if (automaticFailoverTransition && session.previewSession.empty())
                    continue;
                ::shutdown(fd, SHUT_RDWR);
                if (session.upstreamFd >= 0) ::shutdown(session.upstreamFd, SHUT_RDWR);
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(asyncStopMutex_);
        stoppingAbortRefs_[id] = {
            state->nativeRelay, state->nativeHlsInput, state->nativeSrtInput,
            state->nativeRtspInput, state->nativeRtmpInput
        };
    }
    state->monitorStop.store(true);
    state->testPatternStop.store(true);
    // Signal relay shutdown BEFORE joining SRT/HLS/RTSP/RTMP producers.
    // A producer can be parked in enqueueHttpData() waiting for queue space;
    // joining it first deadlocks because only the relay shutdown wakes it.
    // requestStop() is nonblocking and does not destroy producer callbacks.
    if (state->nativeRelay) state->nativeRelay->requestStop();
    if (state->nativeHttpHub) {
        if (automaticFailoverTransition) {
            state->nativeHttpHub->flushPending();
            std::cerr << "NATIVE HTTP FAILOVER PRESERVE"
                      << " stream_id=" << id
                      << " action=retain_tcp_sessions"
                      << std::endl;
        } else {
            state->nativeHttpHub->close();
        }
    }
    if (state->nativePreviewHub) state->nativePreviewHub->close();
    if (state->nativeHlsInput) state->nativeHlsInput->stop();
    if (state->nativeSrtInput) state->nativeSrtInput->stop();
    if (state->nativeRtspInput) state->nativeRtspInput->stop();
    if (state->nativeRtmpInput) state->nativeRtmpInput->stop();
    // After the producer joins, finish relay teardown and detach the shared
    // DVB reader. requestStop() above already released any blocked producers.
    if (state->nativeRelay) state->nativeRelay->stop();
    sharedDvbInputs.unsubscribe(id);
    // V347: the monitor can sample nativeSrtOutputs while the old relay
    // winds down. Join it before moving those unique_ptrs into the parked
    // failover socket pool, avoiding a concurrent vector move/read race.
    if (state->monitorThread.joinable()) state->monitorThread.join();
    if (state->nativePreviewTranscoder) {
        state->nativePreviewTranscoder->reset();
    }
    if (state->nativeTranscoder) {
        state->nativeTranscoder->setDecodedVideoObserver({});
        state->nativeTranscoder->setEncodedAudioObserver({});
        state->nativeTranscoder->reset();
    }
    for (auto& variant : state->hlsAbrVariants) {
        if (variant && variant->transcoder) variant->transcoder->reset();
    }
    if (state->testPatternThread.joinable()) state->testPatternThread.join();
    // Automatic input transitions must NOT disconnect the SRT player. Once
    // the relay is stopped, park its output workers before destroying state.
    if (automaticFailoverTransition && !state->nativeSrtOutputs.empty()) {
        std::vector<std::unique_ptr<dvbstreamer5::media::srt::NativeSrtOutput>>
            replaced;
        {
            std::lock_guard<std::mutex> lock(managerMutex);
            auto& parked = parkedFailoverSrtOutputs[id];
            replaced = std::move(parked);
            parked = std::move(state->nativeSrtOutputs);
        }
        replaced.clear();
        std::cerr << "NATIVE SRT FAILOVER PARK"
                  << " stream_id=" << id
                  << " action=preserve_peers"
                  << std::endl;
    }
    for (auto& output : state->nativeSrtOutputs) if (output) output->stop();
    for (auto& output : state->nativeRtspOutputs) if (output) output->stop();
    for (auto& output : state->nativeRtmpOutputs) if (output) output->stop();
    if (!automaticFailoverTransition) {
        // Only explicit Stop/Restart ends a live playlist; input failover
        // leaves the exact same segmenter alive until the new source starts.
        if (state->nativeHlsSegmenter) state->nativeHlsSegmenter->stop();
        for (auto& variant : state->hlsAbrVariants) {
            if (variant && variant->segmenter) variant->segmenter->stop();
        }
        if (state->nativeCmafSegmenter) state->nativeCmafSegmenter->stop();
        std::lock_guard<std::mutex> lock(managerMutex);
        failoverHttpHubs.erase(id);
        failoverOutputClocks.erase(id);
        failoverPrimaryPrograms.erase(id);
        failoverHlsOutputs.erase(id);
        failoverCmafOutputs.erase(id);
        failoverHlsAbrOutputs.erase(id);
    }
    if (state->monitorThread.joinable()) state->monitorThread.join();
    // Persist the last complete EPG before this stopped/restarted/OnDemand
    // collector is destroyed. Never perform file I/O under managerMutex.
    if (state->epgCollector && !state->epgCacheKey.empty()) {
        (void)state->epgCollector->persistToDisk(state->epgCacheKey);
        // The EPG API and IPTV XMLTV sidecar must remain usable while a
        // source is temporarily offline or an OnDemand stream is idle.
        std::lock_guard<std::mutex> cacheLock(dormantEpgMutex_);
        dormantEpg_[id] = {state->epgCacheKey, state->epgCollector};
    }
    state->running.store(false);
    state->active.store(false);
    CardManager::instance().releaseService(id);
    {
        std::lock_guard<std::mutex> lock(asyncStopMutex_);
        stoppingAbortRefs_.erase(id);
    }
    return true;
}

// HTTP stop requests must not wait for a missing SRT/HLS/HTTP source, a
// blocked codec teardown, or failoverActionMutex. The stream's launch time
// identifies the exact generation to stop, so an old queued request cannot
// destroy a newer instance after a manual restart.
void StreamManager::requestStopInputs(const StopAbortRefs& refs) noexcept {
    // All objects are held alive while the atomic cancellation signal runs.
    // Never join threads, free objects or release CA readers from this path.
    if (auto relay = refs.relay.lock()) relay->requestStop();
    if (auto hls = refs.hls.lock()) hls->requestStop();
    if (auto srt = refs.srt.lock()) srt->requestStop();
    if (auto rtsp = refs.rtsp.lock()) rtsp->requestStop();
    if (auto rtmp = refs.rtmp.lock()) rtmp->requestStop();
}

void StreamManager::forceCancelStreamIo(const std::string& id) {
    StopAbortRefs refs;
    {
        std::lock_guard<std::mutex> lock(managerMutex);
        const auto it = streams.find(id);
        if (it != streams.end() && it->second) {
            const auto& state = it->second;
            state->monitorStop.store(true);
            state->testPatternStop.store(true);
            refs = {state->nativeRelay, state->nativeHlsInput,
                    state->nativeSrtInput, state->nativeRtspInput,
                    state->nativeRtmpInput};
        }
        // Shutdown is a nonblocking wake-up; never close(fd) here because
        // HTTP session owners still own their descriptors and may reuse them.
        for (const auto& [fd, session] : httpClients) {
            if (session.streamId != id) continue;
            ::shutdown(fd, SHUT_RDWR);
            if (session.upstreamFd >= 0)
                ::shutdown(session.upstreamFd, SHUT_RDWR);
        }
    }
    {
        std::lock_guard<std::mutex> lock(asyncStopMutex_);
        const auto it = stoppingAbortRefs_.find(id);
        if (it != stoppingAbortRefs_.end()) refs = it->second;
    }
    requestStopInputs(refs);
    std::cerr << "NATIVE FORCE STOP SIGNAL stream_id=" << id
              << " action=abort_input_and_relay" << std::endl;
}

// Always nonblocking: explicit user Stop is authoritative even if the
// source/codec shutdown is currently stuck. A forced repeat signals the
// same generation (including one already moved into the teardown worker).
bool StreamManager::stopStreamAsync(const std::string& id, bool force) {
    if (id.empty()) return false;
    std::chrono::steady_clock::time_point generation;
    bool activeGeneration = false;
    {
        std::lock_guard<std::mutex> lock(managerMutex);
        const auto it = streams.find(id);
        if (it != streams.end() && it->second) {
            generation = it->second->startedAt;
            activeGeneration = true;
        }
    }
    bool pending = false;
    bool enqueue = false;
    {
        std::lock_guard<std::mutex> lock(asyncStopMutex_);
        if (asyncStopShutdown_) return false;
        // Suppress automatic failover and OnDemand reactivation.
        operatorStoppedStreams_.insert(id);
        pending = asyncStopPending_.count(id) != 0;
        if (!activeGeneration && !pending) return false;
        if (force) forceStopRequested_.insert(id);
        if (!pending && activeGeneration) {
            asyncStopPending_.insert(id);
            try {
                asyncStopQueue_.push_back({id, generation, force});
                enqueue = true;
            } catch (...) {
                asyncStopPending_.erase(id);
                forceStopRequested_.erase(id);
                throw;
            }
        }
    }
    if (force) forceCancelStreamIo(id);
    if (enqueue) asyncStopCv_.notify_one();
    return true;
}

bool StreamManager::isForceStopRequested(const std::string& id) const {
    std::lock_guard<std::mutex> lock(asyncStopMutex_);
    return forceStopRequested_.count(id) != 0;
}

bool StreamManager::isStopPending(const std::string& id) const {
    std::lock_guard<std::mutex> lock(asyncStopMutex_);
    return asyncStopPending_.count(id) != 0;
}
bool StreamManager::isOperatorStopped(const std::string& id) const {
    std::lock_guard<std::mutex> lock(asyncStopMutex_);
    return operatorStoppedStreams_.count(id) != 0;
}
void StreamManager::clearOperatorStop(const std::string& id) {
    std::lock_guard<std::mutex> lock(asyncStopMutex_);
    operatorStoppedStreams_.erase(id);
}

void StreamManager::processAsyncStopRequests() {
    for (;;) {
        AsyncStopRequest request;
        {
            std::unique_lock<std::mutex> lock(asyncStopMutex_);
            asyncStopCv_.wait(lock, [this] {
                return asyncStopShutdown_ || !asyncStopQueue_.empty();
            });
            if (asyncStopQueue_.empty()) return;
            request = std::move(asyncStopQueue_.front());
            asyncStopQueue_.pop_front();
        }
        try {
            // Keep manual stop serialized with automatic failover; only this
            // background thread waits for potentially long source transitions.
            // Normal stop remains serialized with automatic input failover.
            // An escalated force-stop bypasses a stuck failover transition,
            // while the operator-stopped gate blocks its new start.
            std::unique_lock<std::mutex> actionLock(
                failoverActionMutex, std::defer_lock);
            while (!actionLock.owns_lock() &&
                   !request.force && !isForceStopRequested(request.id)) {
                if (actionLock.try_lock()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            StreamConfig stoppedConfig;
            bool currentGenerationPresent = false;
            {
                std::lock_guard<std::mutex> lock(managerMutex);
                const auto it = streams.find(request.id);
                if (it != streams.end() && it->second) {
                    stoppedConfig = it->second->config;
                    currentGenerationPresent = true;
                    if (it->second->startedAt != request.generation) {
                        // An automatic source transition may have completed
                        // after Stop was queued. Stop the newest generation;
                        // explicit Start is rejected while Stop is pending.
                        std::cerr << "NATIVE ASYNC STOP NEW GENERATION stream_id="
                                  << request.id << std::endl;
                    }
                }
            }
            if (currentGenerationPresent) {
                const auto stopBegan = std::chrono::steady_clock::now();
                std::cerr << "NATIVE ASYNC STOP BEGIN stream_id="
                          << request.id << std::endl;
                {
                    std::lock_guard<std::mutex> lock(failoverMutex);
                    failoverEntries.erase(request.id);
                }
                const bool completed = stopStream(request.id);
                std::cerr << "NATIVE ASYNC STOP COMPLETE stream_id="
                          << request.id
                          << " stopped=" << (completed ? 1 : 0)
                          << " elapsed_ms="
                          << std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - stopBegan).count()
                          << std::endl;
                if (completed) {
                    sendTelegramStreamState(
                        telegramNotifier, configManager, stoppedConfig, "🟡",
                        telegramText(configManager, "Поток остановлен", "Stream stopped"),
                        telegramText(configManager,
                            "Остановлен пользователем через панель",
                            "Stopped by user from control panel"));
                }
            }
        } catch (const std::exception& ex) {
            std::cerr << "NATIVE ASYNC STOP ERROR stream_id=" << request.id
                      << " reason=" << ex.what() << std::endl;
        } catch (...) {
            std::cerr << "NATIVE ASYNC STOP ERROR stream_id=" << request.id
                      << " reason=unknown" << std::endl;
        }
        {
            std::lock_guard<std::mutex> lock(asyncStopMutex_);
            asyncStopPending_.erase(request.id);
            forceStopRequested_.erase(request.id);
        }
    }
}

void StreamManager::stopAll() {
    // A scheduled/manual global stop must not race a single-tile failover.
    std::lock_guard<std::mutex> actionLock(failoverActionMutex);
    {
        std::lock_guard<std::mutex> lock(failoverMutex);
        failoverEntries.clear();
    }
    std::vector<std::string> ids;
    {
        std::lock_guard<std::mutex> lock(managerMutex);
        for (const auto& item : streams) ids.push_back(item.first);
    }
    for (const auto& id : ids) stopStream(id);
    sharedDvbInputs.stopAll();
    if (mptsOutputManager) mptsOutputManager->stopAll();
    CardManager::instance().releaseAll();
}

bool StreamManager::isStreamActive(const std::string& id) {
    std::lock_guard<std::mutex> lock(managerMutex);
    const auto it = streams.find(id);
    return it != streams.end() && it->second->active.load();
}

bool StreamManager::ensureOnDemandStream(const std::string& id, const std::string& source,
                                         std::string* error) {
    if (error) error->clear();
    StreamConfig cfg;
    bool found = false;
    for (const auto& candidate : configManager.config.streams) {
        if (candidate.id == id) {
            cfg = candidate;
            found = true;
            break;
        }
    }
    if (!found) {
        if (error) *error = "stream is not configured";
        return false;
    }
    if (toLower(cfg.activationMode) != "ondemand") return true;

    std::lock_guard<std::mutex> demandLock(onDemandMutex);
    // An operator Stop overrides an existing HLS/HTTP/SRT subscriber.
    // Playback requests cannot silently turn the tile back on.
    if (isOperatorStopped(id)) {
        if (error) *error = "stream manually stopped by operator";
        return false;
    }
    const auto now = std::chrono::steady_clock::now();
    onDemandLastActivity[id] = now;
    const bool activeAlready = isStreamActive(id);
    if (!activeAlready) {
        // A fresh generation must never inherit the prior player's HLS state.
        onDemandHlsStartupAt.erase(id);
        onDemandHlsFirstMediaServed.erase(id);
    }
    if (source == "hls" && cfg.transcodeEnabled &&
        !onDemandHlsFirstMediaServed.count(id) &&
        !onDemandHlsStartupAt.count(id) &&
        (!activeAlready || onDemandStartedStreams.count(id))) {
        onDemandHlsStartupAt[id] = now;
        std::cerr << "ONDEMAND HLS STARTUP GRACE"
                  << " stream=" << id
                  << " grace_s="
                  << dvbstreamer5::media::ondemand::kHlsColdStartGrace.count()
                  << " condition=until_first_media_fetch"
                  << std::endl;
    }
    if (activeAlready) return true;

    std::string startError;
    if (!startStream(cfg, &startError)) {
        // A manual start may win the race between the pre-check and startStream().
        if (isStreamActive(id)) return true;
        if (error) *error = startError.empty() ? "failed to start on-demand stream" : startError;
        std::cerr << "ONDEMAND ACTIVATE FAILED stream=" << id
                  << " source=" << source << std::endl;
        onDemandHlsStartupAt.erase(id);
        return false;
    }
    onDemandStartedStreams.insert(id);
    std::cerr << "ONDEMAND ACTIVATE stream=" << id
              << " source=" << source
              << " ca=" << (cfg.conditionalAccessClient.empty() ? "fta" : "managed")
              << std::endl;
    return true;
}

void StreamManager::markOnDemandHlsFirstMedia(const std::string& id) {
    if (id.empty()) return;
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(onDemandMutex);
    const auto startup = onDemandHlsStartupAt.find(id);
    if (startup == onDemandHlsStartupAt.end()) return;
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - startup->second).count();
    onDemandHlsFirstMediaServed.insert(id);
    onDemandHlsStartupAt.erase(startup);
    onDemandLastActivity[id] = now;
    std::cerr << "ONDEMAND HLS FIRST MEDIA"
              << " stream=" << id
              << " elapsed_ms=" << elapsedMs
              << " action=restore_normal_30s_idle"
              << std::endl;
}

void StreamManager::configureRestartSchedule() {
    {
        std::lock_guard<std::mutex> lock(restartScheduleMutex);
        restartScheduleConfig = configManager.config.restartSchedule;
    }
    restartScheduleCv.notify_all();
}

void StreamManager::monitorGlobalRestartSchedule() {
    // Skip the current minute at daemon startup. No catch-up restarts and no
    // duplicate restarts during repeated polls / DST fall-back wall time.
    std::time_t initialTime = std::time(nullptr);
    std::time_t lastMinute = initialTime / 60;
    int localDate = -1;
    std::set<int> executedSlots;
    while (!restartScheduleStop.load(std::memory_order_acquire)) {
        StreamRestartSchedule policy;
        {
            std::unique_lock<std::mutex> lock(restartScheduleMutex);
            restartScheduleCv.wait_for(lock, std::chrono::seconds(2), [this] {
                return restartScheduleStop.load(std::memory_order_acquire);
            });
            if (restartScheduleStop.load(std::memory_order_acquire)) break;
            policy = restartScheduleConfig;
        }
        const std::time_t now = std::time(nullptr);
        const std::time_t minuteKey = now / 60;
        if (minuteKey == lastMinute) continue;
        lastMinute = minuteKey;
        std::tm localTime {};
        if (localtime_r(&now, &localTime) == nullptr) continue;
        const int dateKey = dvbstreamer5::schedule::dateKey(localTime);
        if (dateKey != localDate) {
            executedSlots.clear();
            localDate = dateKey;
        }
        if (!dvbstreamer5::schedule::matchesLocalTime(policy.enabled,
                policy.weekdays, policy.hours, policy.minute, localTime)) continue;
        const int weekday = localTime.tm_wday == 0 ? 7 : localTime.tm_wday;
        const int slot = dvbstreamer5::schedule::minuteSlot(localTime);
        if (!executedSlots.insert(slot).second) continue;

        // Copy live configs while protected, then release before any restart;
        // do not start streams that the operator previously stopped manually.
        std::vector<StreamConfig> active;
        {
            std::lock_guard<std::mutex> lock(managerMutex);
            for (const auto& [id, stream] : streams) {
                (void)id;
                if (stream && stream->active.load(std::memory_order_acquire))
                    active.push_back(stream->config);
            }
        }
        std::cerr << "GLOBAL STREAM RESTART SCHEDULE date=" << dateKey
                  << " weekday=" << weekday << " at=" << localTime.tm_hour << ':'
                  << localTime.tm_min << " active=" << active.size() << std::endl;
        for (const auto& cfg : active) {
            if (restartScheduleStop.load(std::memory_order_acquire)) break;
            if (!isStreamActive(cfg.id)) continue;
            std::string error;
            const bool ok = restartStream(cfg, &error);
            std::cerr << "GLOBAL STREAM RESTART stream=" << cfg.id
                      << " result=" << (ok ? "ok" : "failed")
                      << (error.empty() ? "" : " error=" + error) << std::endl;
        }
    }
}

void StreamManager::monitorOnDemandStreams() {
    // HLS clients fetch short-lived playlist/segment resources rather than keeping
    // a persistent socket open. Ten seconds was too aggressive and could stop an
    // actively watched channel between requests, producing visible HLS stalls.
    constexpr auto kIdleGrace = std::chrono::seconds(30);
    while (!onDemandMonitorStop.load(std::memory_order_acquire)) {
        for (int i = 0; i < 4 && !onDemandMonitorStop.load(std::memory_order_acquire); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        if (onDemandMonitorStop.load(std::memory_order_acquire)) break;

        std::lock_guard<std::mutex> demandLock(onDemandMutex);
        const auto now = std::chrono::steady_clock::now();
        for (auto it = onDemandStartedStreams.begin(); it != onDemandStartedStreams.end();) {
            const std::string id = *it;
            bool stillOnDemand = false;
            for (const auto& cfg : configManager.config.streams) {
                if (cfg.id == id) {
                    stillOnDemand = toLower(cfg.activationMode) == "ondemand";
                    break;
                }
            }
            if (!stillOnDemand) {
                onDemandLastActivity.erase(id);
                onDemandHlsStartupAt.erase(id);
                onDemandHlsFirstMediaServed.erase(id);
                it = onDemandStartedStreams.erase(it);
                continue;
            }

            bool hasHttpClient = false;
            bool hasRecentHlsMedia = false;
            {
                std::lock_guard<std::mutex> lock(managerMutex);
                // A connected authenticated preview is also a real viewer.
                // Only live TCP sockets count; a closed preview never keeps
                // an OnDemand stream alive.
                for (const auto& [fd, session] : httpClients) {
                    (void)fd;
                    if (session.streamId == id) {
                        hasHttpClient = true;
                        break;
                    }
                }
                // Unlike HTTP TS, HLS has no long-lived socket. A successful
                // segment request is recorded in adHocSessions; retain the
                // stream for a bounded playback/buffer gap since that media,
                // even if no playlist request has arrived in the last 30 s.
                for (const auto& [key, session] : adHocSessions) {
                    (void)key;
                    if (session.streamId == id && session.protocol == "hls" &&
                        dvbstreamer5::media::ondemand::keepHlsOnDemandFromRecentMedia(
                            session.lastActivity, now)) {
                        hasRecentHlsMedia = true;
                        break;
                    }
                }
            }
            const auto activity = onDemandLastActivity.find(id);
            const bool recentActivity = activity != onDemandLastActivity.end() &&
                now - activity->second < kIdleGrace;
            // V340: HLS has no persistent TCP connection while waiting for
            // its first IDR-aligned segment. The player may stop retrying
            // 503s before media is ready, then the normal 30-s inactivity
            // check would tear down the transcoder before it can warm up.
            // Retain the stream for at most 120 s from its FIRST HLS request,
            // ending the grace immediately after the first .ts/.m4s fetch.
            const auto hlsStartup = onDemandHlsStartupAt.find(id);
            const bool pendingFirstHlsMedia =
                hlsStartup != onDemandHlsStartupAt.end() &&
                dvbstreamer5::media::ondemand::keepTranscodedHlsOnDemandStarting(
                    hlsStartup->second, now,
                    onDemandHlsFirstMediaServed.count(id) != 0);
            if (hasHttpClient || hasRecentHlsMedia ||
                recentActivity || pendingFirstHlsMedia) {
                ++it;
                continue;
            }

            const bool hlsStartupExpired =
                hlsStartup != onDemandHlsStartupAt.end() &&
                onDemandHlsFirstMediaServed.count(id) == 0;
            it = onDemandStartedStreams.erase(it);
            onDemandLastActivity.erase(id);
            onDemandHlsStartupAt.erase(id);
            onDemandHlsFirstMediaServed.erase(id);
            if (isStreamActive(id)) {
                std::cerr << "ONDEMAND DEACTIVATE stream=" << id
                          << " reason=no-clients idle_s=30"
                          << " hls_startup_expired=" << (hlsStartupExpired ? 1 : 0)
                          << std::endl;
                // An OnDemand idle Stop is authoritative even if an input
                // failover probe is about to complete.
                std::lock_guard<std::mutex> actionLock(failoverActionMutex);
                {
                    std::lock_guard<std::mutex> lock(failoverMutex);
                    failoverEntries.erase(id);
                }
                stopStream(id);
            }
        }
    }
}

std::vector<std::string> StreamManager::activeStreams() {
    std::vector<std::string> result;
    std::lock_guard<std::mutex> lock(managerMutex);
    for (const auto& [id, state] : streams) if (state->active.load()) result.push_back(id);
    return result;
}

std::map<std::string, StreamState*> StreamManager::snapshot() {
    std::map<std::string, StreamState*> result;
    std::lock_guard<std::mutex> lock(managerMutex);
    for (auto& [id, state] : streams) result[id] = state.get();
    return result;
}

bool StreamManager::addHttpClient(const std::string& id, int fd, const std::string& clientIp,
                                  const std::string& previewSession,
                                  const std::string& authSessionId) {
    std::shared_ptr<dvbstreamer5::media::network::NativePreviewHub> hub;
    {
        std::lock_guard<std::mutex> lock(managerMutex);
        const auto it = streams.find(id);
        if (it == streams.end() || !it->second->active.load()) {
            std::cerr << "NATIVE HTTP TS CLIENT rejected"
                      << " stream_id=" << id
                      << " reason=stream_inactive"
                      << std::endl;
            ::close(fd);
            return false;
        }
        // previewSession is set only for the private browser preview endpoint.
        // Ordinary HTTP MPEG-TS must consume the production-output hub.
        hub = previewSession.empty() ? it->second->nativeHttpHub
                                     : it->second->nativePreviewHub;
        if (!hub) {
            std::cerr << "NATIVE HTTP TS CLIENT rejected"
                      << " stream_id=" << id
                      << " reason=hub_unavailable"
                      << std::endl;
            ::close(fd);
            return false;
        }
    }
    // V10.8.285: the HTTP MPEG-TS client is a TCP connection receiving
    // many 1316-byte writes from a SOCK_SEQPACKET production hub. Unlike SRT,
    // TCP may coalesce writes behind Nagle/delayed ACK. Explicitly opt in one
    // affected *public* HTTP stream; browser preview and all other sessions
    // preserve their existing socket options.
    const char* noDelayId =
        std::getenv("DVBSTREAMER5_HTTP_MPEGTS_NODELAY_STREAM_ID");
    // Default to low-latency TCP for public MPEG-TS. An explicitly supplied
    // stream ID still scopes the option to one stream for A/B testing; set
    // the value to "off" to disable it for every stream.
    const bool enableNoDelay = previewSession.empty() &&
        (!noDelayId || !*noDelayId || id == noDelayId);
    bool noDelayActive = false;
    if (enableNoDelay) {
        int value = 1;
        if (::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
                         &value, sizeof(value)) == 0) {
            noDelayActive = true;
            std::cerr << "NATIVE HTTP TS TCP_NODELAY"
                      << " stream_id=" << id
                      << " enabled=1"
                      << std::endl;
        } else {
            std::cerr << "NATIVE HTTP TS TCP_NODELAY"
                      << " stream_id=" << id
                      << " enabled=0"
                      << " error=" << std::strerror(errno)
                      << std::endl;
        }
    }

    // Public HTTP MPEG-TS can see TCP scheduling stalls even with Nagle off.
    // Give the kernel sufficient socket buffering for short delivery bursts,
    // while leaving browser preview and the producer pipeline unchanged.
    if (previewSession.empty()) {
        const int desiredTcpSendBytes = 2 * 1024 * 1024;
        if (::setsockopt(fd, SOL_SOCKET, SO_SNDBUF,
                         &desiredTcpSendBytes, sizeof(desiredTcpSendBytes)) != 0) {
            std::cerr << "NATIVE HTTP TS TCP SEND BUFFER"
                      << " stream_id=" << id
                      << " error=" << std::strerror(errno) << std::endl;
        }
        int actualBytes = 0;
        socklen_t len = sizeof(actualBytes);
        if (::getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &actualBytes, &len) == 0) {
            std::cerr << "NATIVE HTTP TS TCP SEND BUFFER"
                      << " stream_id=" << id
                      << " bytes=" << actualBytes << std::endl;
        }
    }

    std::string subscribeError;
    const int upstreamFd = hub->subscribe(subscribeError);
    if (upstreamFd < 0) {
        std::cerr << "NATIVE HTTP TS CLIENT rejected"
                  << " stream_id=" << id
                  << " reason=subscribe_failed"
                  << " detail=" << subscribeError
                  << std::endl;
        ::close(fd);
        return false;
    }
    std::cerr << "NATIVE HTTP TS CLIENT attach"
              << " stream_id=" << id
              << " kind=" << (previewSession.empty() ? "public" : "preview")
              << " fd=" << fd
              << " hub_fd=" << upstreamFd
              << " tcp_nodelay=" << (noDelayActive ? 1 : 0)
              << std::endl;
    {
        std::lock_guard<std::mutex> lock(managerMutex);
        httpClients[fd] = {id, normalizeIpAddress(clientIp), "mpegts",
                           std::chrono::steady_clock::now(), upstreamFd, previewSession};
        httpClients[fd].authSessionId=authSessionId;
    }
    if(!authSessionId.empty())
        dvbstreamer5::stream_auth::SessionRegistry::instance().attach(authSessionId);
    const auto accepted=httpEpollDispatcher_->add(fd,upstreamFd,
        [this,hub,fd,upstreamFd,id,authSessionId,noDelayActive](
            const char* reason,std::uint64_t sent){
            {
                std::lock_guard<std::mutex> lock(managerMutex);
                httpClients.erase(fd);
            }
            hub->unsubscribe(upstreamFd);
            ::close(upstreamFd);
            ::close(fd);
            if(!authSessionId.empty())
                dvbstreamer5::stream_auth::SessionRegistry::instance().detach(authSessionId);
            std::cerr<<"NATIVE HTTP TS CLIENT detach"
                     <<" stream_id="<<id
                     <<" fd="<<fd<<" reason="<<reason
                     <<" bytes_sent="<<sent
                     <<" tcp_nodelay="<<(noDelayActive?1:0)
                     <<" delivery=epoll_shared"
                     <<std::endl;
        },
        [authSessionId]{
            if(authSessionId.empty())return true;
            auto& registry=dvbstreamer5::stream_auth::SessionRegistry::instance();
            if(!registry.isAllowed(authSessionId))return false;
            registry.touch(authSessionId);
            return true;
        });
    if(!accepted){
        if(!authSessionId.empty())
            dvbstreamer5::stream_auth::SessionRegistry::instance().detach(authSessionId);
        {
            std::lock_guard<std::mutex> lock(managerMutex);
            httpClients.erase(fd);
        }
        hub->unsubscribe(upstreamFd);
        ::close(upstreamFd);
        ::close(fd);
        return false;
    }
    return true;
}

bool StreamManager::closePreviewSession(const std::string& id, const std::string& previewSession) {
    bool closed = false;
    std::lock_guard<std::mutex> lock(managerMutex);
    for (const auto& [fd, session] : httpClients) {
        if (session.streamId == id && session.previewSession == previewSession) {
            ::shutdown(fd, SHUT_RDWR);
            if (session.upstreamFd >= 0) ::shutdown(session.upstreamFd, SHUT_RDWR);
            closed = true;
        }
    }
    return closed;
}

bool StreamManager::addStreamSession(const std::string& streamId, const std::string& clientIp,
                                     const std::string& protocol) {
    if (streamId.empty() || clientIp.empty()) return false;
    std::lock_guard<std::mutex> lock(managerMutex);
    const std::string ip = normalizeIpAddress(clientIp);
    // HLS performs many independent HTTP requests per viewer. Reuse one logical
    // session per protocol/stream/client and only refresh its activity timestamp;
    // otherwise every .m3u8/.ts request allocates another map node for two minutes.
    const std::string key = protocol + ":" + streamId + ":" + ip;
    const bool callbackDriven = protocol == "srt" || protocol == "rtsp" || protocol == "rtmp";
    auto existing = adHocSessions.find(key);
    if (callbackDriven && existing != adHocSessions.end()) {
        // Separate SRT/RTSP peers may share an IP and stream. One peer
        // disconnecting must not hide the others from subscriber monitoring.
        ++existing->second.connectionCount;
    }
    auto& session = adHocSessions[key];
    session.streamId = streamId;
    session.clientIp = ip;
    session.protocol = protocol;
    session.lastActivity = std::chrono::steady_clock::now();
    session.upstreamFd = -1;
    session.previewSession.clear();
    return true;
}

bool StreamManager::removeStreamSession(const std::string& streamId, const std::string& clientIp,
                                        const std::string& protocol) {
    const std::string ip = normalizeIpAddress(clientIp);
    bool removed = false;
    std::lock_guard<std::mutex> lock(managerMutex);
    for (auto it = adHocSessions.begin(); it != adHocSessions.end();) {
        if (it->second.streamId == streamId && it->second.clientIp == ip && it->second.protocol == protocol) {
            const bool callbackDriven = protocol == "srt" || protocol == "rtsp" || protocol == "rtmp";
            if (callbackDriven && it->second.connectionCount > 1) {
                --it->second.connectionCount;
                ++it;
            } else {
                it = adHocSessions.erase(it);
            }
            removed = true;
        } else ++it;
    }
    return removed;
}

void StreamManager::pruneExpiredAdHocSessionsLocked(std::chrono::steady_clock::time_point now) {
    for (auto it = adHocSessions.begin(); it != adHocSessions.end();) {
        // Connection-oriented transports report real disconnects via callbacks.
        // Their timestamp is set at connection time and is NOT a keepalive:
        // expiring them after two minutes hid live SRT subscribers.
        const auto& protocol = it->second.protocol;
        const bool callbackDriven = protocol == "srt" || protocol == "rtsp" || protocol == "rtmp";
        if (!callbackDriven && now - it->second.lastActivity > kAdHocSessionTtl)
            it = adHocSessions.erase(it);
        else ++it;
    }
}

size_t StreamManager::activeHttpSessions(const std::string& clientIp) const {
    const std::string ip = normalizeIpAddress(clientIp);
    size_t count = 0;
    std::lock_guard<std::mutex> lock(managerMutex);
    for (const auto& [fd, session] : httpClients) { (void)fd; if (session.clientIp == ip) ++count; }
    for (const auto& [key, session] : adHocSessions) { (void)key; if (session.clientIp == ip) count += session.connectionCount; }
    return count;
}

size_t StreamManager::activeSubscriberSessions(const SubscriberConfig& subscriber) {
    if (!subscriber.enabled) return 0;
    const std::string primary = normalizeIpAddress(subscriber.primaryIp);
    const std::string backup = normalizeIpAddress(subscriber.backupIp);
    auto matches = [&](const HttpClientSession& session) {
        const bool ip = session.clientIp == primary || (!backup.empty() && session.clientIp == backup);
        const bool stream = std::find(subscriber.streamIds.begin(), subscriber.streamIds.end(), session.streamId) != subscriber.streamIds.end();
        return ip && stream;
    };
    size_t count = 0;
    std::lock_guard<std::mutex> lock(managerMutex);
    pruneExpiredAdHocSessionsLocked(std::chrono::steady_clock::now());
    for (const auto& [fd, session] : httpClients) { (void)fd; if (matches(session)) ++count; }
    for (const auto& [key, session] : adHocSessions) { (void)key; if (matches(session)) count += session.connectionCount; }
    return count;
}

std::vector<ActiveStreamSession> StreamManager::activeStreamSessions() {
    std::map<std::string, ActiveStreamSession> grouped;
    std::lock_guard<std::mutex> lock(managerMutex);
    const auto now = std::chrono::steady_clock::now();
    pruneExpiredAdHocSessionsLocked(now);
    auto add = [&](const HttpClientSession& session) {
        const std::string key = session.clientIp + "\n" + session.streamId + "\n" + session.protocol;
        auto& out = grouped[key];
        out.streamId = session.streamId;
        out.clientIp = session.clientIp;
        out.protocol = session.protocol;
        out.connections += session.connectionCount;
        // Grouped HTTP sockets retain the oldest currently connected socket.
        // For HLS, the initial logical session survives segment requests.
        const auto age = std::chrono::duration_cast<std::chrono::seconds>(
            now - session.connectedAt).count();
        if (age > 0) out.connectedSeconds = std::max(
            out.connectedSeconds, static_cast<std::uint64_t>(age));
    };
    for (const auto& [fd, session] : httpClients) { (void)fd; add(session); }
    for (const auto& [key, session] : adHocSessions) { (void)key; add(session); }
    std::vector<ActiveStreamSession> result;
    for (auto& [key, session] : grouped) { (void)key; result.push_back(std::move(session)); }
    return result;
}

size_t StreamManager::disconnectAuthSession(const std::string& sessionId) {
    if(sessionId.empty())return 0;
    std::lock_guard<std::mutex> lock(managerMutex);
    std::size_t count=0;
    for(const auto& [fd,session]:httpClients){
        if(session.authSessionId!=sessionId)continue;
        ::shutdown(fd,SHUT_RDWR);
        if(session.upstreamFd>=0)::shutdown(session.upstreamFd,SHUT_RDWR);
        ++count;
    }
    return count;
}

size_t StreamManager::resetHttpSessions(const std::string& clientIp) {
    const std::string ip = normalizeIpAddress(clientIp);
    size_t count = 0;
    std::lock_guard<std::mutex> lock(managerMutex);
    for (const auto& [fd, session] : httpClients) {
        if (session.clientIp == ip) {
            ::shutdown(fd, SHUT_RDWR);
            if (session.upstreamFd >= 0) ::shutdown(session.upstreamFd, SHUT_RDWR);
            ++count;
        }
    }
    for (auto it = adHocSessions.begin(); it != adHocSessions.end();) {
        if (it->second.clientIp == ip) { it = adHocSessions.erase(it); ++count; }
        else ++it;
    }
    return count;
}

bool StreamManager::isClientAllowedForStream(const std::string& streamId, const std::string& clientIp) const {
    if (!configManager.subscribers.filteringEnabled) return true;
    const std::string ip = normalizeIpAddress(clientIp);
    if (std::find(configManager.subscribers.blockedIps.begin(), configManager.subscribers.blockedIps.end(), ip) != configManager.subscribers.blockedIps.end()) return false;
    for (const auto& subscriber : configManager.subscribers.subscribers) {
        if (!subscriber.enabled) continue;
        const bool ipMatch = normalizeIpAddress(subscriber.primaryIp) == ip ||
            (!subscriber.backupIp.empty() && normalizeIpAddress(subscriber.backupIp) == ip);
        const bool streamMatch = std::find(subscriber.streamIds.begin(), subscriber.streamIds.end(), streamId) != subscriber.streamIds.end();
        if (ipMatch && streamMatch) return true;
    }
    return false;
}

size_t StreamManager::enforceSubscriberAccess() {
    size_t count = 0;
    std::lock_guard<std::mutex> lock(managerMutex);
    for (const auto& [fd, session] : httpClients) {
        // V381: backend-authorized HTTP clients are never disconnected by
        // changes to the local IP subscriber lists.
        if (dvbstreamer5::stream_auth::enabled(configManager.config,session.streamId)) continue;
        if (!isClientAllowedForStream(session.streamId, session.clientIp)) {
            ::shutdown(fd, SHUT_RDWR);
            if (session.upstreamFd >= 0) ::shutdown(session.upstreamFd, SHUT_RDWR);
            ++count;
        }
    }
    for (auto it = adHocSessions.begin(); it != adHocSessions.end();) {
        const auto& protocol = it->second.protocol;
        if (dvbstreamer5::stream_auth::enabled(
                configManager.config,it->second.streamId) &&
            (protocol == "http" || protocol == "hls" || protocol == "srt")) {
            ++it;
            continue;
        }
        if (!isClientAllowedForStream(it->second.streamId, it->second.clientIp)) {
            it = adHocSessions.erase(it); ++count;
        } else ++it;
    }
    return count;
}

size_t StreamManager::restartSrtOutputsForStreams(const std::vector<std::string>& streamIds) {
    std::vector<StreamConfig> restart;
    for (const auto& cfg : configManager.config.streams) {
        if (std::find(streamIds.begin(), streamIds.end(), cfg.id) != streamIds.end() && isStreamActive(cfg.id)) restart.push_back(cfg);
    }
    size_t count = 0; for (const auto& cfg : restart) { std::string error; if (restartStream(cfg, &error)) ++count; }
    return count;
}
size_t StreamManager::restartAllSrtOutputs() {
    std::vector<StreamConfig> restart;
    for (const auto& cfg : configManager.config.streams) {
        bool uses = toLower(cfg.inputUri).rfind("srt://",0)==0 || toLower(cfg.outputType)=="srt";
        for (const auto& out : cfg.additionalOutputs) uses = uses || toLower(out.outputType)=="srt";
        if (uses && isStreamActive(cfg.id)) restart.push_back(cfg);
    }
    size_t count = 0; for (const auto& cfg : restart) { std::string error; if (restartStream(cfg, &error)) ++count; }
    return count;
}

void StreamManager::configureMptsOutputs() {
    if (mptsOutputManager) mptsOutputManager->configure(configManager.config.mptsOutputs, configManager.config.streams);
}

bool StreamManager::startMptsOutput(const std::string& id, std::string* error) {
    return mptsOutputManager && mptsOutputManager->start(id, error);
}

bool StreamManager::stopMptsOutput(const std::string& id) {
    return mptsOutputManager && mptsOutputManager->stop(id);
}

Json::Value StreamManager::mptsSnapshot() const {
    return mptsOutputManager ? mptsOutputManager->snapshot() : Json::Value(Json::arrayValue);
}

void StreamManager::persistAllDvbEpg() {
    std::vector<std::pair<std::string,
        std::shared_ptr<dvbstreamer5::media::epg::DvbEpgCollector>>> items;
    {
        std::lock_guard<std::mutex> lock(managerMutex);
        for (const auto& [id, state] : streams) {
            (void)id;
            if (state && state->epgCollector && !state->epgCacheKey.empty())
                items.emplace_back(state->epgCacheKey, state->epgCollector);
        }
    }
    // Copies were taken under the mutex; disk access is outside it.
    for (const auto& [key, collector] : items)
        if (collector) (void)collector->persistToDisk(key);
}

void StreamManager::configureExternalXmltvSources() {
    if (externalXmltvManager_)
        externalXmltvManager_->configure(configManager.config.xmltvSources);
}

bool StreamManager::refreshExternalXmltv(const std::string& id) {
    return externalXmltvManager_ && externalXmltvManager_->refresh(id);
}

Json::Value StreamManager::externalXmltvStatus() const {
    return externalXmltvManager_
        ? externalXmltvManager_->status() : Json::Value(Json::objectValue);
}

Json::Value StreamManager::externalXmltvChannels() const {
    return externalXmltvManager_
        ? externalXmltvManager_->channels() : Json::Value(Json::arrayValue);
}

Json::Value StreamManager::mediaTracks(const std::string& id) const {
    Json::Value result(Json::objectValue);
    result["stream_id"]=id;
    result["active"]=false;
    result["input_scanned"]=false;
    result["output_scanned"]=false;
    result["input"]=Json::Value(Json::arrayValue);
    result["output"]=Json::Value(Json::arrayValue);
    std::shared_ptr<DvbSubtitlePassthrough> inputMonitor,outputMonitor;
    {
        std::lock_guard<std::mutex> lock(managerMutex);
        const auto it=streams.find(id);
        if(it!=streams.end() && it->second) {
            result["active"]=it->second->active.load();
            inputMonitor=it->second->dvbSubtitleMonitor;
            outputMonitor=it->second->outputMediaMonitor;
        }
    }
    const auto append=[&](const std::shared_ptr<DvbSubtitlePassthrough>& monitor,
                          const char* listKey,const char* scanKey) {
        if(!monitor)return;
        result[scanKey]=monitor->hasParsedPmt();
        for(const auto& track:monitor->availableMediaTracks()) {
            Json::Value item(Json::objectValue);
            item["pid"]=track.pid;
            item["kind"]=track.kind;
            item["codec"]=track.codec;
            item["language"]=track.language;
            result[listKey].append(item);
        }
    };
    append(inputMonitor,"input","input_scanned");
    append(outputMonitor,"output","output_scanned");
    return result;
}

Json::Value StreamManager::dvbSubtitleTracks(const std::string& id) const {
    Json::Value result(Json::objectValue);
    result["stream_id"] = id;
    result["active"] = false;
    result["scanned"] = false;
    result["tracks"] = Json::Value(Json::arrayValue);
    std::shared_ptr<DvbSubtitlePassthrough> monitor;
    {
        std::lock_guard<std::mutex> lock(managerMutex);
        const auto it = streams.find(id);
        if (it != streams.end() && it->second) {
            result["active"] = it->second->active.load();
            monitor = it->second->dvbSubtitleMonitor;
        }
    }
    if (!monitor) return result;
    result["scanned"] = monitor->hasParsedPmt();
    for (const auto& track : monitor->availableTracks()) {
        Json::Value item(Json::objectValue);
        item["id"] = track.id;
        item["language"] = track.language;
        item["pid"] = track.pid;
        item["subtitle_type"] = track.subtitleType;
        item["composition_page_id"] = track.compositionPageId;
        item["ancillary_page_id"] = track.ancillaryPageId;
        result["tracks"].append(std::move(item));
    }
    return result;
}

dvbstreamer5::media::network::DvbNitInfo StreamManager::dvbNitInfo(
    int adapter, int frontend) const {
    return sharedDvbInputs.nitInfo(adapter, frontend);
}

Json::Value StreamManager::epgSnapshot(const std::string& id) const {
    Json::Value root(Json::objectValue);
    root["stream_id"] = id;

    const StreamConfig* cfg = nullptr;
    for (const auto& stream : configManager.config.streams) {
        if (stream.id == id) { cfg = &stream; break; }
    }
    if (!cfg) {
        root["error"] = "stream not found";
        root["enabled"] = false;
        return root;
    }

    root["enabled"] = cfg->epgEnabled;
    root["days"] = cfg->epgDays;
    root["mode"] = cfg->epgMode;
    root["source"] = "none";
    if (!cfg->epgEnabled) return root;

    std::shared_ptr<dvbstreamer5::media::epg::DvbEpgCollector> collector;
    if (cfg->epgMode != "xmltv") {
        std::lock_guard<std::mutex> lock(managerMutex);
        const auto it = streams.find(id);
        if (it != streams.end() && it->second)
            collector = it->second->epgCollector;
    }
    if (!collector && cfg->epgMode != "xmltv") {
        const std::string key = cfg->id + "|" + cfg->inputUri + "|" +
            std::to_string(cfg->inputServiceId);
        std::lock_guard<std::mutex> lock(dormantEpgMutex_);
        auto it = dormantEpg_.find(id);
        if (it != dormantEpg_.end() && it->second.first == key)
            collector = it->second.second;
        else {
            // Read persistent EIT only once per inactive channel. This does
            // not start DVB hardware and is safe for OnDemand/failed feeds.
            auto cached = std::make_shared<dvbstreamer5::media::epg::DvbEpgCollector>(
                cfg->inputServiceId <= 0xffffU
                    ? static_cast<std::uint16_t>(cfg->inputServiceId) : 0,
                cfg->epgDays);
            (void)cached->restoreFromDisk(key);
            dormantEpg_[id] = {key, cached};
            collector = std::move(cached);
        }
    }

    const std::int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const auto eventJson = [](const dvbstreamer5::media::epg::DvbEpgEvent& event) {
        Json::Value value(Json::objectValue);
        value["service_id"] = event.serviceId;
        value["event_id"] = event.eventId;
        value["start_utc"] = Json::Int64(event.startUtc);
        value["end_utc"] = Json::Int64(
            event.startUtc + static_cast<std::int64_t>(event.durationSeconds));
        value["duration_seconds"] = event.durationSeconds;
        value["running_status"] = event.runningStatus;
        value["free_ca"] = event.freeCa;
        value["title"] = event.title;
        value["description"] = event.description;
        return value;
    };
    Json::Value events(Json::arrayValue);
    const dvbstreamer5::media::epg::DvbEpgEvent* current = nullptr;
    const dvbstreamer5::media::epg::DvbEpgEvent* next = nullptr;
    // Keep the backing event storage alive until current/next are serialized.
    // A block-local snapshot left these pointers dangling after the if block,
    // causing Json::LogicError or SIGSEGV when DVB EIT output requested EPG.
    dvbstreamer5::media::epg::DvbEpgSnapshot snapshot;
    if (collector) {
        snapshot = collector->snapshot(512);
        root["service_id"] = snapshot.serviceId;
        root["sections"] = Json::UInt64(snapshot.sectionsParsed);
        for (const auto& event : snapshot.events) {
            events.append(eventJson(event));
            const std::int64_t end = event.startUtc +
                static_cast<std::int64_t>(event.durationSeconds);
            if (!current && event.startUtc <= now && now < end)
                current = &event;
            if (!next && event.startUtc > now) next = &event;
        }
    }
    const bool dvbHasRelevantProgramme = current || next;
    bool useExternal = cfg->epgMode == "xmltv" ||
        (cfg->epgMode == "auto" && !dvbHasRelevantProgramme);
    if (useExternal && externalXmltvManager_ && !cfg->xmltvChannelId.empty()) {
        Json::Value external = externalXmltvManager_->snapshot(
            cfg->xmltvSourceId, cfg->xmltvChannelId, cfg->epgDays);
        if (!external.empty()) {
            events = std::move(external);
            root["source"] = "xmltv";
            root["service_id"] = cfg->serviceId;
            current = nullptr; next = nullptr;
            for (const auto& item : events) {
                const std::int64_t begin = item.get("start_utc", 0).asInt64();
                const std::int64_t end = item.get("end_utc", 0).asInt64();
                if (!root.isMember("now") && begin <= now && now < end)
                    root["now"] = item;
                if (!root.isMember("next") && begin > now)
                    root["next"] = item;
            }
        }
    }
    if (root["source"].asString() != "xmltv") {
        root["source"] = collector ? "dvb" : "none";
        if (current) root["now"] = eventJson(*current);
        if (next) root["next"] = eventJson(*next);
    }
    root["active"] = static_cast<bool>(collector);
    root["available"] = !events.empty();
    root["events"] = std::move(events);
    return root;
}

Json::Value StreamManager::queueMemorySnapshot() const {
    Json::Value root;
    root["engine"] = "native";
    root["stream_count"] = Json::UInt64(streams.size());
    return root;
}


// V342: source restoration probes and background per-channel failover worker.
#include "StreamManagerFailover.inc"
