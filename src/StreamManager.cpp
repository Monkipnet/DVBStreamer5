#include "StreamManager.h"

#include "CardManager.h"
#include "CaBackend.h"
#include "DvbSatellite.h"
#include "TranscoderModule.h"
#include "mpts/MptsOutputManager.h"
#include "protocols/SrtVpsProfile.h"
#include "utils.h"
#include "media/NativeSampleAes.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

constexpr auto kAdHocSessionTtl = std::chrono::minutes(2);

bool writeAll(int fd, const char* data, std::size_t size) {
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t written = ::send(fd, data + offset, size - offset, MSG_NOSIGNAL);
        if (written > 0) {
            offset += static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

std::string cleanInterface(std::string value) {
    if (value == "auto" || value == "Auto" || value == "0.0.0.0") return {};
    return value;
}

bool startsWith(const std::string& value, const char* prefix) {
    return value.rfind(prefix, 0) == 0;
}

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
    auto codecs = [&cfg]() {
        const std::string v = toLower(cfg.transcodeVideoCodec);
        const std::string a = toLower(cfg.transcodeAudioCodec);
        std::string c = (v == "hevc" || v == "h265") ? "hvc1.1.6.L120.B0" : "avc1.640028";
        if (a == "aac") c += ",mp4a.40.2";
        else if (a == "mp3") c += ",mp4a.6B";
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
            << ",FRAME-RATE=25.000,CODECS=\"" << codecs << "\"\n";
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
      mptsOutputManager(std::make_unique<MptsOutputManager>()) {
    configureMptsOutputs();
    onDemandMonitorThread = std::thread(&StreamManager::monitorOnDemandStreams, this);
}

StreamManager::~StreamManager() {
    onDemandMonitorStop.store(true, std::memory_order_release);
    if (onDemandMonitorThread.joinable()) onDemandMonitorThread.join();
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
    const std::string input = toLower(normalizeInputUri(cfg.inputUri));
    const std::string mode = toLower(cfg.inputMode);
    if (cfg.testPattern || mode == "test" || input == "test://bars" || input == "testsrc://bars" || input == "bars://hd") {
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
    std::string reason;
    if (!isNativeInputSupported(streamConfig, reason)) {
        if (error) *error = reason;
        return false;
    }

    std::vector<dvbstreamer5::media::network::NativeUdpRelayOutputConfig> nativeOutputs;
    struct SrtOutputSpec {
        std::string host; int port = 0; std::string mode; std::string iface;
        int latency = 120; std::string passphrase; std::string streamId; int pbkeylen = 16;
    };
    std::vector<SrtOutputSpec> srtOutputSpecs;
    struct RtspOutputSpec { std::string host; int port=8554; };
    std::vector<RtspOutputSpec> rtspOutputSpecs;
    struct RtmpOutputSpec { std::string uri; };
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
        if (type == "rtsp") { rtspOutputSpecs.push_back({host, port > 0 ? port : 8554}); return true; }
        if (type == "rtmp" || type == "youtube") {
            std::string uri = host;
            const std::string lo = toLower(uri);
            if (type == "youtube" && lo.rfind("rtmp",0) != 0) uri = "rtmp://a.rtmp.youtube.com/live2/" + host;
            else if (type == "rtmp" && lo.rfind("rtmp",0) != 0) uri = "rtmp://" + host + ":" + std::to_string(port > 0 ? port : 1935) + "/live/" + streamConfig.id;
            rtmpOutputSpecs.push_back({uri}); return true;
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
    if (!CardManager::instance().reserveService(streamConfig, &caError)) {
        if (error) *error = caError.empty() ? "CAM reservation failed" : caError;
        return false;
    }

    auto state = std::make_unique<StreamState>();
    std::uint64_t effectivePrimaryVideoBitrate = streamConfig.transcodeVideoBitrate;
    state->config = streamConfig;
    state->activeInputUri = streamConfig.inputUri;
    state->nativeHttpHub = std::make_shared<dvbstreamer5::media::network::NativePreviewHub>();
    state->nativePreviewHub = std::make_shared<dvbstreamer5::media::network::NativePreviewHub>();
    state->nativeRelay = std::make_unique<dvbstreamer5::media::network::NativeUdpRelay>();

    // Browser preview must not depend on the production codec. mpegts.js /
    // MediaSource playback is reliable with AVC + AAC, while DVB services can
    // legitimately carry MPEG-2, HEVC, MP2 or AC-3. Keep a dedicated,
    // low-bitrate CPU pipeline whose output is used only by preview.ts.
    // V10.8.69 restores the proven V10.8.41 preview path; production transcoding
    // remains independent and may still use hardware acceleration.
    state->nativePreviewTranscoder =
        std::make_unique<dvbstreamer5::media::transcode::NativeTranscoderPipeline>();
    {
        dvbstreamer5::media::transcode::NativeTranscoderConfig previewTc;
        previewTc.videoCodec = "h264";
        previewTc.videoEncoder = "cpu";
        previewTc.audioCodec = "aac";
        previewTc.width = 1280;
        previewTc.height = 720;
        previewTc.fps = 25.0;
        previewTc.videoBitrate = 1800000ULL;
        previewTc.audioBitrate = 128000ULL;
        previewTc.deinterlace = true;
        // Browser preview is square-pixel AVC. Keep 1280x720 for
        // anamorphic DVB MPEG-2 instead of reverting to 720x576.
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

        std::string previewError;
        if (!state->nativePreviewTranscoder->initialize(previewTc, previewError)) {
            std::cerr << "NATIVE BROWSER PREVIEW disabled stream="
                      << streamConfig.name
                      << " error=" << (previewError.empty()
                          ? "preview transcoder initialization failed" : previewError)
                      << std::endl;
            state->nativePreviewTranscoder.reset();
        } else {
            std::cerr << "NATIVE BROWSER PREVIEW ready stream="
                      << streamConfig.name
                      << " codec=h264/aac size=1280x720 dar=16:9"
                      << " encoder=cpu video_kbps=1800 audio_kbps=128"
                      << std::endl;
        }
    }

    if (streamConfig.transcodeEnabled) {
        state->nativeTranscoder = std::make_unique<dvbstreamer5::media::transcode::NativeTranscoderPipeline>();
        dvbstreamer5::media::transcode::NativeTranscoderConfig tc;
        tc.videoCodec = toLower(streamConfig.transcodeVideoCodec);
        tc.videoEncoder = toLower(streamConfig.transcodeVideoEncoder);
        tc.audioCodec = toLower(streamConfig.transcodeAudioCodec);
        if (!TranscoderModule::resolutionSize(streamConfig.transcodeResolution, tc.width, tc.height)) {
            CardManager::instance().releaseService(streamConfig.id);
            if (error) *error = "invalid native transcode resolution: " + streamConfig.transcodeResolution;
            return false;
        }
        tc.audioBitrate = streamConfig.transcodeAudioBitrate;
        tc.videoBitrate = streamConfig.transcodeVideoBitrate;
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
        effectivePrimaryVideoBitrate = tc.videoBitrate;
        tc.serviceId = static_cast<std::uint16_t>(streamConfig.serviceId > 0 && streamConfig.serviceId <= 0xffff ? streamConfig.serviceId : 1);
        tc.videoPid = static_cast<std::uint16_t>(streamConfig.videoPid > 0 && streamConfig.videoPid <= 0x1ffe ? streamConfig.videoPid : 0x0100);
        tc.audioPid = static_cast<std::uint16_t>(streamConfig.audioPid > 0 && streamConfig.audioPid <= 0x1ffe ? streamConfig.audioPid : 0x0101);
        tc.muxBitrate = streamConfig.cbr ? streamConfig.targetBitrate : 0;
        tc.serviceName = streamConfig.serviceName.empty() ? streamConfig.name : streamConfig.serviceName;
        tc.serviceProvider = streamConfig.serviceProvider;
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
                variant->segmenter = std::make_unique<dvbstreamer5::media::hls::NativeHlsSegmenter>();
                auto abrTc = tc;
                abrTc.width = variant->width;
                abrTc.height = variant->height;
                abrTc.videoBitrate = variant->videoBitrate;
                abrTc.muxBitrate = variant->muxBitrate;
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
                                const std::uint64_t sourcePixels =
                                    static_cast<std::uint64_t>(frame->width) *
                                    static_cast<std::uint64_t>(frame->height);
                                const std::uint64_t primaryPixelsRequested =
                                    static_cast<std::uint64_t>(
                                        std::max(0, primaryGeometry.first)) *
                                    static_cast<std::uint64_t>(
                                        std::max(0, primaryGeometry.second));

                                if (sourcePixels > 0 &&
                                    primaryPixelsRequested > sourcePixels) {
                                    (void)statePtr->nativeTranscoder
                                        ->setOutputGeometryIfUnconfigured(
                                            frame->width, frame->height);
                                    primaryGeometry =
                                        statePtr->nativeTranscoder
                                            ->configuredOutputGeometry();
                                }

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
                    [statePtr](const dvbstreamer5::media::codec::EncodedAudioFrame& frame,
                               std::uint64_t duration90k) {
                        if (!statePtr) return;
                        std::lock_guard<std::mutex> abrLock(statePtr->hlsAbrMutex);
                        for (auto& variant : statePtr->hlsAbrVariants) {
                            if (!variant || !variant->enabled ||
                                !variant->transcoder)
                                continue;
                            (void)variant->transcoder
                                ->pushEncodedAudioFrame(frame, duration90k);
                        }
                    });
                std::cerr << "NATIVE HLS ABR shared_decode=1 shared_audio=1 source_demuxers=1 renditions="
                          << (state->hlsAbrVariants.size() + 1) << std::endl;
            }
        }
    }
    const std::string normalizedInput = normalizeInputUri(streamConfig.inputUri);
    const std::string inputMode = toLower(streamConfig.inputMode);
    const bool hlsInput = inputMode == "hls" || toLower(normalizedInput).find(".m3u8") != std::string::npos ||
        toLower(normalizedInput).rfind("hls://", 0) == 0;
    const bool srtInput = toLower(normalizedInput).rfind("srt://", 0) == 0;
    const bool rtspInput = toLower(normalizedInput).rfind("rtsp://", 0) == 0;
    const bool rtmpInput = toLower(normalizedInput).rfind("rtmp://", 0) == 0 || toLower(normalizedInput).rfind("rtmps://", 0) == 0;
    const bool testInput = streamConfig.testPattern || inputMode == "test" ||
        toLower(normalizedInput) == "test://bars" || toLower(normalizedInput) == "testsrc://bars" ||
        toLower(normalizedInput) == "bars://hd";
    if (hlsInput) state->nativeHlsInput = std::make_unique<dvbstreamer5::media::hls::NativeHlsInput>();
    if (srtInput) state->nativeSrtInput = std::make_unique<dvbstreamer5::media::srt::NativeSrtInput>();
    if (rtspInput) state->nativeRtspInput = std::make_unique<dvbstreamer5::media::rtsp::NativeRtspInput>();
    if (rtmpInput) state->nativeRtmpInput = std::make_unique<dvbstreamer5::media::rtmp::NativeRtmpInput>();
    for (const auto& spec : srtOutputSpecs) {
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
        if (srtConfig.mode == "listener") srtConfig.host = "0.0.0.0";
        const std::string currentStreamId = streamConfig.id;
        if (!output->start(
                srtConfig,
                [this, currentStreamId](const std::string& peerIp) {
                    return isClientAllowedForStream(currentStreamId, peerIp);
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
        state->nativeSrtOutputs.push_back(std::move(output));
    }
    for (const auto& spec : rtspOutputSpecs) {
        auto output = std::make_unique<dvbstreamer5::media::rtsp::NativeRtspOutput>();
        dvbstreamer5::media::rtsp::OutputConfig cfg; cfg.bindAddress=cleanInterface(spec.host); if(cfg.bindAddress.empty())cfg.bindAddress="0.0.0.0"; cfg.port=spec.port; cfg.streamName=streamConfig.id;
        std::string e; const std::string currentStreamId=streamConfig.id;
        if(!output->start(cfg,[this,currentStreamId](const std::string&ip){return isClientAllowedForStream(currentStreamId,ip);},
            [this,currentStreamId](const std::string&ip){if(!ip.empty())addStreamSession(currentStreamId,ip,"rtsp");},
            [this,currentStreamId](const std::string&ip){if(!ip.empty())removeStreamSession(currentStreamId,ip,"rtsp");},e)) {
            CardManager::instance().releaseService(streamConfig.id); if(error)*error=e.empty()?"native RTSP output failed":e; return false;
        }
        state->nativeRtspOutputs.push_back(std::move(output));
    }
    for (const auto& spec : rtmpOutputSpecs) {
        auto output=std::make_unique<dvbstreamer5::media::rtmp::NativeRtmpOutput>(); dvbstreamer5::media::rtmp::EndpointConfig cfg; cfg.uri=spec.uri; cfg.bindAddress=cleanInterface(streamConfig.interfaceAddress);
        std::string e; if(!output->start(cfg,[statePtr=state.get()](const std::string&st){if(statePtr)statePtr->statusMessage="RTMP "+st;},e)) { CardManager::instance().releaseService(streamConfig.id); if(error)*error=e.empty()?"native RTMP output failed":e; return false; }
        state->nativeRtmpOutputs.push_back(std::move(output));
    }
    if (hasHlsOutput) {
        if(toLower(streamConfig.hlsContainer)=="cmaf") state->nativeCmafSegmenter=std::make_unique<dvbstreamer5::media::cmaf::NativeCmafSegmenter>();
        else state->nativeHlsSegmenter = std::make_unique<dvbstreamer5::media::hls::NativeHlsSegmenter>();
    }

    dvbstreamer5::media::network::NativeUdpRelayConfig relay;
    relay.inputUri = testInput ? "external://test" : (hlsInput ? "external://hls" : (srtInput ? "external://srt" : (rtspInput ? "external://rtsp" : (rtmpInput ? "external://rtmp" : normalizedInput))));
    relay.externallyFedInput = testInput || hlsInput || srtInput || rtspInput || rtmpInput;
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
    relay.paceObservedTransport = streamConfig.cbr && streamConfig.targetBitrate > 0;

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
        relay.paceObservedTransport = false;
        relay.dvbTuneConfig.adapter = dvbParams.adapter;
        relay.dvbTuneConfig.frontend = dvbParams.frontend;
        relay.dvbTuneConfig.frequencyKHz = dvbParams.frequencyKHz;
        relay.dvbTuneConfig.symbolRateK = dvbParams.symbolRateK;
        relay.dvbTuneConfig.polarity = dvbParams.polarity;
        relay.dvbTuneConfig.deliverySystem = dvbParams.deliverySystem;
        relay.dvbTuneConfig.modulation = dvbParams.modulation;
        relay.dvbTuneConfig.fec = dvbParams.fec;
        relay.dvbTuneConfig.diseqcSource = dvbParams.diseqcSource;
        relay.dvbTuneConfig.lnbLof1KHz = dvbParams.lnbLof1KHz;
        relay.dvbTuneConfig.lnbLof2KHz = dvbParams.lnbLof2KHz;
        relay.dvbTuneConfig.lnbSlofKHz = dvbParams.lnbSlofKHz;
        relay.dvbTuneConfig.streamId = dvbParams.streamId;
        // The shared reader always takes the complete multiplex.  Per-channel
        // PAT/PMT/remap/CA filtering remains independent in each NativeUdpRelay.
        relay.dvbTuneConfig.pids = "8192";
    }

    relay.remapEnabled = streamConfig.remapEnabled ||
        (sharedDvbInputSource && streamConfig.inputServiceId > 0);
    if (relay.remapEnabled) {
        const uint32_t outSid = streamConfig.remapEnabled ? streamConfig.serviceId : streamConfig.inputServiceId;
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
        relay.remapConfig.serviceName = streamConfig.serviceName.empty() ? streamConfig.name : streamConfig.serviceName;
        relay.remapConfig.serviceProvider = streamConfig.serviceProvider;
    }

    if (!streamConfig.conditionalAccessClient.empty()) {
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
            std::lock_guard<std::mutex> abrLock(statePtr->hlsAbrMutex);
            for (auto& variant : statePtr->hlsAbrVariants) {
                if (!variant || !variant->enabled || variant->failed ||
                    !variant->transcoder || !variant->segmenter) continue;
                std::vector<std::uint8_t> encoded;
                std::string abrError;
                if (!variant->transcoder->pollOutput(encoded, abrError)) {
                    variant->failed = true;
                    variant->lastError = abrError.empty() ? "native ABR output poll failed" : abrError;
                    std::cerr << "NATIVE HLS ABR ERROR name=" << variant->name
                              << " error=" << variant->lastError << std::endl;
                    continue;
                }
                if (!encoded.empty() && !variant->segmenter->push(encoded.data(), encoded.size())) {
                    variant->failed = true;
                    variant->lastError = variant->segmenter->lastError();
                    std::cerr << "NATIVE HLS ABR ERROR name=" << variant->name
                              << " error=" << variant->lastError << std::endl;
                }
            }
        };
    }
    if (state->nativePreviewTranscoder) {
        auto previousInputObserver = relay.observeInputTransport;
        auto* previewTranscoder = state->nativePreviewTranscoder.get();
        auto previewHubForTranscode = state->nativePreviewHub;
        StreamState* previewState = state.get();
        relay.observeInputTransport =
            [previousInputObserver, previewTranscoder, previewHubForTranscode, previewState](
                const std::uint8_t* data, std::size_t size) {
                if (previousInputObserver) previousInputObserver(data, size);
                if (!previewTranscoder || !previewHubForTranscode || !previewState ||
                    previewState->previewTranscodeFailed.load(std::memory_order_acquire) ||
                    previewHubForTranscode->subscriberCount() == 0) {
                    return;
                }

                std::vector<std::uint8_t> browserTs;
                std::string previewError;
                if (!previewTranscoder->process(data, size, browserTs, previewError)) {
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
                    previewHubForTranscode->publish(
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

    auto httpHub = state->nativeHttpHub;
    auto previewHub = state->nativePreviewHub;
    const bool previewPassthrough = !state->nativePreviewTranscoder;
    auto* hlsSegmenter = state->nativeHlsSegmenter.get();
    auto* cmafSegmenter = state->nativeCmafSegmenter.get();
    auto* mpts = mptsOutputManager.get();
    std::vector<dvbstreamer5::media::srt::NativeSrtOutput*> srtOutputs; for (auto& output : state->nativeSrtOutputs) srtOutputs.push_back(output.get());
    std::vector<dvbstreamer5::media::rtsp::NativeRtspOutput*> rtspOutputs; for (auto& output : state->nativeRtspOutputs) rtspOutputs.push_back(output.get());
    std::vector<dvbstreamer5::media::rtmp::NativeRtmpOutput*> rtmpOutputs; for (auto& output : state->nativeRtmpOutputs) rtmpOutputs.push_back(output.get());
    const std::string streamId = streamConfig.id;
    StreamState* outputStatsState = state.get();
    relay.observeTransport = [httpHub, previewHub, previewPassthrough, hlsSegmenter, cmafSegmenter, mpts, srtOutputs, rtspOutputs, rtmpOutputs, streamId, outputStatsState](const uint8_t* data, std::size_t size) {
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
        if (hlsSegmenter) hlsSegmenter->push(data, size);
        if (cmafSegmenter) cmafSegmenter->push(data, size);
        for (auto* output : srtOutputs) if (output) output->push(data, size);
        for (auto* output : rtspOutputs) if (output) output->push(data, size);
        for (auto* output : rtmpOutputs) if (output) output->push(data, size);
        // Normal HTTP MPEG-TS clients consume the finished production transport.
        // They must never subscribe to the browser-preview hub because doing so
        // would wake the dedicated H.264/AAC preview transcoder.
        if (httpHub) httpHub->publish(data, size);
        if (previewPassthrough && previewHub) previewHub->publish(data, size);
        if (mpts) mpts->pushBytes(streamId, data, size);
    };

    if (state->nativeHlsSegmenter) {
        dvbstreamer5::media::hls::NativeHlsSegmenterConfig hlsConfig;
        hlsConfig.directory = streamConfig.hlsArchiveEnabled
            ? std::filesystem::path(streamConfig.hlsArchivePath) / streamConfig.id
            : std::filesystem::path("/tmp/dvbstreamer5-hls") / streamConfig.id;
        hlsConfig.targetDurationSeconds = 2.0;
        hlsConfig.liveWindowSegments = 6;
        hlsConfig.archiveEnabled = streamConfig.hlsArchiveEnabled;
        hlsConfig.archiveHours = streamConfig.hlsArchiveHours;
        hlsConfig.independentSegments =
            streamConfig.transcodeEnabled &&
            toLower(streamConfig.transcodeVideoCodec) != "copy";
        hlsConfig.encryption = toLower(streamConfig.hlsEncryption);
        hlsConfig.keyUri = streamConfig.hlsEncryptionKeyUri;
        hlsConfig.hasKey = dvbstreamer5::media::hls::parseHexKey16(streamConfig.hlsEncryptionKeyHex, hlsConfig.key);
        std::string hlsError;
        if (!state->nativeHlsSegmenter->start(hlsConfig, hlsError)) {
            for (auto& output : state->nativeSrtOutputs) if (output) output->stop();
            CardManager::instance().releaseService(streamConfig.id);
            if (error) *error = hlsError.empty() ? "native HLS output failed" : hlsError;
            return false;
        }

        if (!state->hlsAbrVariants.empty()) {
            const auto baseDir = hlsConfig.directory;
            std::error_code abrEc;
            if (!streamConfig.hlsArchiveEnabled) {
                std::filesystem::remove_all(baseDir / "abr", abrEc);
                abrEc.clear();
                std::filesystem::remove(baseDir / "master.m3u8", abrEc);
                abrEc.clear();
            }
            for (auto& variant : state->hlsAbrVariants) {
                auto variantCfg = hlsConfig;
                variantCfg.directory = baseDir / "abr" / variant->name;
                std::string variantError;
                if (!variant->segmenter->start(variantCfg, variantError)) {
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
        cfg.targetDurationSeconds = 2.0;
        cfg.liveWindowSegments = 6;
        cfg.archiveEnabled = streamConfig.hlsArchiveEnabled;
        cfg.archiveHours = streamConfig.hlsArchiveHours;
        cfg.encryption = enc;
        cfg.keyUri = streamConfig.hlsEncryptionKeyUri;
        cfg.hasKey = dvbstreamer5::media::hls::parseHexKey16(streamConfig.hlsEncryptionKeyHex, cfg.key);
        std::string e;
        if (!state->nativeCmafSegmenter->start(cfg, e)) {
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
                    [relayPtr](const std::uint8_t* data, std::size_t size) {
                        return relayPtr->pushInput(data, size);
                    },
                    [relayPtr](const std::string& finishError) {
                        relayPtr->finishInput(
                            finishError.empty()
                                ? "shared DVB source stopped"
                                : finishError);
                    },
                    sharedDvbError)) {
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
    return true;
}

void StreamManager::monitorNativeStream(StreamState* state) {
    uint64_t lastIn = 0, lastOut = 0, lastPayloadOut = 0, lastCc = 0;
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
        const uint64_t outRate = (out - lastOut) * 8;
        // Outputs such as an SRT listener can have zero sentBytes until a
        // receiver connects. Never fall back to the raw DVB multiplex rate;
        // use the channel pipeline bitrate instead.
        state->outputBitrate.store(outRate ? outRate : payloadRate);
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

        // For long-running HTTP inputs the relay intentionally stays alive while
        // reconnecting.  Surface that state instead of leaving the UI falsely
        // ONLINE with empty bitrate fields.  As soon as data resumes, restore
        // the normal running status.
        const std::string relayErrorNow = relay->lastError();
        if (in == 0 && !relayErrorNow.empty()) {
            state->statusMessage = relayErrorNow;
        } else if (in > 0 && lastIn == 0) {
            const std::string normalized = normalizeInputUri(state->config.inputUri);
            const std::string mode = toLower(state->config.inputMode);
            const bool hls = mode == "hls" || toLower(normalized).find(".m3u8") != std::string::npos ||
                toLower(normalized).rfind("hls://", 0) == 0;
            const bool srt = toLower(normalized).rfind("srt://", 0) == 0;
            const bool rtsp = toLower(normalized).rfind("rtsp://", 0) == 0;
            const bool rtmp = toLower(normalized).rfind("rtmp://", 0) == 0 || toLower(normalized).rfind("rtmps://", 0) == 0;
            state->statusMessage = hls ? "running (native HLS input)" :
                (srt ? "running (native SRT input)" :
                (rtsp ? "running (native RTSP input)" :
                (rtmp ? "running (native RTMP input)" : "running (native media engine)")));
            if (state->config.transcodeEnabled) state->statusMessage += " + native transcoder";
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
            if (state->nativeHlsSegmenter) state->nativeHlsSegmenter->stop();
            for (auto& variant : state->hlsAbrVariants) {
                if (variant && variant->segmenter) variant->segmenter->stop();
            }
            state->running.store(false);
            state->active.store(false);
            const std::string relayError = relay->lastError();
            state->statusMessage = relayError.empty() ? "native input stopped" : relayError;
            break;
        }
    }
}

bool StreamManager::restartStream(const StreamConfig& cfg, std::string* error) {
    stopStream(cfg.id);
    return startStream(cfg, error);
}

bool StreamManager::stopStream(const std::string& id) {
    std::unique_ptr<StreamState> state;
    {
        std::lock_guard<std::mutex> lock(managerMutex);
        auto it = streams.find(id);
        if (it == streams.end()) return false;
        state = std::move(it->second);
        streams.erase(it);
        for (const auto& [fd, session] : httpClients) {
            if (session.streamId == id) {
                ::shutdown(fd, SHUT_RDWR);
                if (session.upstreamFd >= 0) ::shutdown(session.upstreamFd, SHUT_RDWR);
            }
        }
    }
    state->monitorStop.store(true);
    state->testPatternStop.store(true);
    if (state->nativeHttpHub) state->nativeHttpHub->close();
    if (state->nativePreviewHub) state->nativePreviewHub->close();
    if (state->nativeHlsInput) state->nativeHlsInput->stop();
    if (state->nativeSrtInput) state->nativeSrtInput->stop();
    if (state->nativeRtspInput) state->nativeRtspInput->stop();
    if (state->nativeRtmpInput) state->nativeRtmpInput->stop();
    // Stop the per-service relay first so a subscriber callback blocked on its
    // external-input queue wakes immediately; then detach it from the shared
    // transponder reader. The last subscriber closes dvr0 and the demux filter.
    if (state->nativeRelay) state->nativeRelay->stop();
    sharedDvbInputs.unsubscribe(id);
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
    for (auto& output : state->nativeSrtOutputs) if (output) output->stop();
    for (auto& output : state->nativeRtspOutputs) if (output) output->stop();
    for (auto& output : state->nativeRtmpOutputs) if (output) output->stop();
    if (state->nativeHlsSegmenter) state->nativeHlsSegmenter->stop();
    for (auto& variant : state->hlsAbrVariants) {
        if (variant && variant->segmenter) variant->segmenter->stop();
    }
    if (state->nativeCmafSegmenter) state->nativeCmafSegmenter->stop();
    if (state->monitorThread.joinable()) state->monitorThread.join();
    state->running.store(false);
    state->active.store(false);
    CardManager::instance().releaseService(id);
    return true;
}

bool StreamManager::stopStreamAsync(const std::string& id) { return stopStream(id); }

void StreamManager::stopAll() {
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
    onDemandLastActivity[id] = std::chrono::steady_clock::now();
    if (isStreamActive(id)) return true;

    std::string startError;
    if (!startStream(cfg, &startError)) {
        // A manual start may win the race between the pre-check and startStream().
        if (isStreamActive(id)) return true;
        if (error) *error = startError.empty() ? "failed to start on-demand stream" : startError;
        std::cerr << "ONDEMAND ACTIVATE FAILED stream=" << id
                  << " source=" << source << std::endl;
        return false;
    }
    onDemandStartedStreams.insert(id);
    std::cerr << "ONDEMAND ACTIVATE stream=" << id
              << " source=" << source
              << " ca=" << (cfg.conditionalAccessClient.empty() ? "fta" : "managed")
              << std::endl;
    return true;
}

void StreamManager::monitorOnDemandStreams() {
    // HLS clients fetch short-lived playlist/segment resources rather than keeping
    // a persistent socket open. Ten seconds was too aggressive and could stop an
    // actively watched channel between requests, producing visible HLS stalls.
    constexpr auto kIdleGrace = std::chrono::seconds(5);
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
                it = onDemandStartedStreams.erase(it);
                continue;
            }

            bool hasHttpClient = false;
            {
                std::lock_guard<std::mutex> lock(managerMutex);
                for (const auto& [fd, session] : httpClients) {
                    (void)fd;
                    if (session.streamId == id && session.previewSession.empty()) {
                        hasHttpClient = true;
                        break;
                    }
                }
            }
            const auto activity = onDemandLastActivity.find(id);
            const bool recentActivity = activity != onDemandLastActivity.end() &&
                now - activity->second < kIdleGrace;
            if (hasHttpClient || recentActivity) {
                ++it;
                continue;
            }

            it = onDemandStartedStreams.erase(it);
            onDemandLastActivity.erase(id);
            if (isStreamActive(id)) {
                std::cerr << "ONDEMAND DEACTIVATE stream=" << id
                          << " reason=no-clients idle_s=5" << std::endl;
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
                                  const std::string& previewSession) {
    std::shared_ptr<dvbstreamer5::media::network::NativePreviewHub> hub;
    {
        std::lock_guard<std::mutex> lock(managerMutex);
        const auto it = streams.find(id);
        if (it == streams.end() || !it->second->active.load()) {
            ::close(fd);
            return false;
        }
        // previewSession is set only for the private browser preview endpoint.
        // Ordinary HTTP MPEG-TS must consume the production-output hub.
        hub = previewSession.empty() ? it->second->nativeHttpHub
                                     : it->second->nativePreviewHub;
        if (!hub) {
            ::close(fd);
            return false;
        }
    }
    std::string subscribeError;
    const int upstreamFd = hub->subscribe(subscribeError);
    if (upstreamFd < 0) {
        ::close(fd);
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(managerMutex);
        httpClients[fd] = {id, normalizeIpAddress(clientIp), "mpegts",
                           std::chrono::steady_clock::now(), upstreamFd, previewSession};
    }
    try {
        std::thread([this, hub, fd, upstreamFd]() {
            std::array<char, 64 * 1024> buffer{};
            for (;;) {
                const ssize_t n = ::read(upstreamFd, buffer.data(), buffer.size());
                if (n > 0) {
                    if (!writeAll(fd, buffer.data(), static_cast<std::size_t>(n))) break;
                    continue;
                }
                if (n < 0 && errno == EINTR) continue;
                break;
            }
            std::lock_guard<std::mutex> lock(managerMutex);
            httpClients.erase(fd);
            hub->unsubscribe(upstreamFd);
            ::close(upstreamFd);
            ::close(fd);
        }).detach();
    } catch (...) {
        std::lock_guard<std::mutex> lock(managerMutex);
        httpClients.erase(fd);
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
            it = adHocSessions.erase(it);
            removed = true;
        } else ++it;
    }
    return removed;
}

void StreamManager::pruneExpiredAdHocSessionsLocked(std::chrono::steady_clock::time_point now) {
    for (auto it = adHocSessions.begin(); it != adHocSessions.end();) {
        if (now - it->second.lastActivity > kAdHocSessionTtl) it = adHocSessions.erase(it);
        else ++it;
    }
}

size_t StreamManager::activeHttpSessions(const std::string& clientIp) const {
    const std::string ip = normalizeIpAddress(clientIp);
    size_t count = 0;
    std::lock_guard<std::mutex> lock(managerMutex);
    for (const auto& [fd, session] : httpClients) { (void)fd; if (session.clientIp == ip) ++count; }
    for (const auto& [key, session] : adHocSessions) { (void)key; if (session.clientIp == ip) ++count; }
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
    for (const auto& [key, session] : adHocSessions) { (void)key; if (matches(session)) ++count; }
    return count;
}

std::vector<ActiveStreamSession> StreamManager::activeStreamSessions() {
    std::map<std::string, ActiveStreamSession> grouped;
    std::lock_guard<std::mutex> lock(managerMutex);
    pruneExpiredAdHocSessionsLocked(std::chrono::steady_clock::now());
    auto add = [&](const HttpClientSession& session) {
        const std::string key = session.clientIp + "\n" + session.streamId + "\n" + session.protocol;
        auto& out = grouped[key];
        out.streamId = session.streamId;
        out.clientIp = session.clientIp;
        out.protocol = session.protocol;
        ++out.connections;
    };
    for (const auto& [fd, session] : httpClients) { (void)fd; add(session); }
    for (const auto& [key, session] : adHocSessions) { (void)key; add(session); }
    std::vector<ActiveStreamSession> result;
    for (auto& [key, session] : grouped) { (void)key; result.push_back(std::move(session)); }
    return result;
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
        if (!isClientAllowedForStream(session.streamId, session.clientIp)) {
            ::shutdown(fd, SHUT_RDWR);
            if (session.upstreamFd >= 0) ::shutdown(session.upstreamFd, SHUT_RDWR);
            ++count;
        }
    }
    for (auto it = adHocSessions.begin(); it != adHocSessions.end();) {
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

Json::Value StreamManager::queueMemorySnapshot() const {
    Json::Value root;
    root["engine"] = "native";
    root["stream_count"] = Json::UInt64(streams.size());
    return root;
}
