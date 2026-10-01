#include "media/NativeTestPatternSource.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <thread>
#include <vector>

namespace dvbstreamer5::media::testpattern {
namespace {

void appendPackets(const std::vector<mpegts::Packet>& packets, std::vector<std::uint8_t>& bytes) {
    const std::size_t old = bytes.size();
    bytes.resize(old + packets.size() * mpegts::kPacketSize);
    for (std::size_t i = 0; i < packets.size(); ++i) {
        std::copy(packets[i].begin(), packets[i].end(),
                  bytes.begin() + static_cast<std::ptrdiff_t>(old + i * mpegts::kPacketSize));
    }
}

std::uint8_t clampByte(int v) {
    return static_cast<std::uint8_t>(std::clamp(v, 0, 255));
}

} // namespace

void NativeTestPatternSource::fillBars(codec::RawVideoFrame& frame, std::uint64_t frameIndex) {
    const int w = frame.width;
    const int h = frame.height;
    frame.i420.assign(static_cast<std::size_t>(w) * h * 3U / 2U, 16);
    auto* y = frame.i420.data();
    auto* u = y + static_cast<std::size_t>(w) * h;
    auto* v = u + static_cast<std::size_t>(w / 2) * (h / 2);

    struct Yuv { int y,u,v; };
    // SMPTE-like 75% bars in studio-range YUV.
    static constexpr std::array<Yuv, 7> bars{{
        {180,128,128}, {168, 44,136}, {145,147, 44}, {133, 63, 52},
        { 63,193,204}, { 51,109,212}, { 28,212,120}
    }};
    for (int row = 0; row < h; ++row) {
        for (int col = 0; col < w; ++col) {
            const int index = std::min(6, (col * 7) / std::max(1, w));
            y[static_cast<std::size_t>(row) * w + col] = clampByte(bars[index].y);
        }
    }
    for (int row = 0; row < h / 2; ++row) {
        for (int col = 0; col < w / 2; ++col) {
            const int index = std::min(6, ((col * 2) * 7) / std::max(1, w));
            u[static_cast<std::size_t>(row) * (w/2) + col] = clampByte(bars[index].u);
            v[static_cast<std::size_t>(row) * (w/2) + col] = clampByte(bars[index].v);
        }
    }

    // Moving white marker proves the generator is live, not a frozen frame.
    const int markerW = std::max(16, w / 32);
    const int markerH = std::max(16, h / 24);
    const int x0 = static_cast<int>((frameIndex * 8) % std::max<std::uint64_t>(1, w - markerW));
    const int y0 = h * 3 / 4;
    for (int row = y0; row < std::min(h, y0 + markerH); ++row)
        for (int col = x0; col < std::min(w, x0 + markerW); ++col)
            y[static_cast<std::size_t>(row) * w + col] = 235;
}

bool NativeTestPatternSource::run(const NativeTestPatternConfig& config,
                                  const Sink& sink,
                                  std::atomic<bool>& stop,
                                  std::string& error) {
    error.clear();
    if (!sink) { error = "test pattern sink is not configured"; return false; }
    if (config.width <= 0 || config.height <= 0 || (config.width & 1) || (config.height & 1)) {
        error = "test pattern requires positive even dimensions";
        return false;
    }

    std::string codecError;
    auto video = codec::createVideoEncoder(mpegts::ElementaryCodec::H264, codecError);
    if (!video || !video->configure(config.width, config.height, config.fps,
                                    config.videoBitrate, codecError)) {
        error = codecError.empty() ? "test pattern H.264 encoder initialization failed" : codecError;
        return false;
    }
    auto audio = codec::createAacEncoder(codecError);
    if (!audio || !audio->configure(48000, 2, config.audioBitrate, codecError)) {
        error = codecError.empty() ? "test pattern AAC encoder initialization failed" : codecError;
        return false;
    }

    mpegts::NativeMpegTsMux mux;
    mpegts::NativeMuxConfig mc;
    mc.serviceId = config.serviceId;
    mc.videoPid = config.videoPid;
    mc.audioPid = config.audioPid;
    mc.targetBitrate = config.muxBitrate;
    mc.serviceName = config.serviceName;
    mc.serviceProvider = config.serviceProvider;
    if (!mux.initialize(mc, error) ||
        !mux.setCodec(mpegts::ElementaryKind::Video, mpegts::ElementaryCodec::H264, error) ||
        !mux.setCodec(mpegts::ElementaryKind::Audio, mpegts::ElementaryCodec::AacAdts, error)) return false;

    const double fps = config.fps > 1.0 ? config.fps : 25.0;
    const std::uint64_t videoStep = static_cast<std::uint64_t>(std::llround(90000.0 / fps));
    constexpr std::uint64_t audioStep = 1920; // 1024 samples @ 48 kHz on the 90 kHz clock.
    std::uint64_t frameIndex = 0;
    std::uint64_t audioPts = 0;
    const auto start = std::chrono::steady_clock::now();

    while (!stop.load(std::memory_order_acquire)) {
        const std::uint64_t videoPts = frameIndex * videoStep;
        std::vector<std::uint8_t> transport;

        codec::RawVideoFrame raw;
        raw.width = config.width;
        raw.height = config.height;
        raw.pts90k = videoPts;
        raw.dts90k = videoPts;
        raw.hasPts = raw.hasDts = true;
        fillBars(raw, frameIndex);
        std::vector<codec::EncodedVideoFrame> encodedVideo;
        if (!video->encode(raw, encodedVideo, error)) return false;
        for (const auto& enc : encodedVideo) {
            mpegts::ElementarySample es;
            es.data = enc.data.data(); es.size = enc.data.size();
            es.pts90k = enc.hasPts ? enc.pts90k : videoPts;
            es.dts90k = enc.hasDts ? enc.dts90k : es.pts90k;
            es.duration90k = videoStep; es.hasPts = es.hasDts = true; es.randomAccess = enc.keyFrame;
            std::vector<mpegts::Packet> packets;
            if (!mux.write(mpegts::ElementaryKind::Video, es, packets, error)) return false;
            appendPackets(packets, transport);
        }

        const std::uint64_t audioLimit = videoPts + videoStep;
        while (audioPts < audioLimit) {
            codec::PcmAudioFrame pcm;
            pcm.sampleRate = 48000; pcm.channels = 2; pcm.pts90k = audioPts; pcm.hasPts = true;
            pcm.samples.assign(1024 * 2, 0); // silence is intentional for deterministic diagnostics.
            std::vector<codec::EncodedAudioFrame> encodedAudio;
            if (!audio->encode(pcm, encodedAudio, error)) return false;
            for (const auto& enc : encodedAudio) {
                mpegts::ElementarySample es;
                es.data = enc.data.data(); es.size = enc.data.size();
                es.pts90k = enc.hasPts ? enc.pts90k : audioPts;
                es.dts90k = es.pts90k; es.duration90k = audioStep; es.hasPts = es.hasDts = true;
                std::vector<mpegts::Packet> packets;
                if (!mux.write(mpegts::ElementaryKind::Audio, es, packets, error)) return false;
                appendPackets(packets, transport);
            }
            audioPts += audioStep;
        }

        if (!transport.empty() && !sink(transport.data(), transport.size())) {
            if (!stop.load(std::memory_order_acquire)) error = "test pattern sink stopped accepting MPEG-TS";
            return stop.load(std::memory_order_acquire);
        }
        ++frameIndex;
        const auto deadline = start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(static_cast<double>(frameIndex) / fps));
        std::this_thread::sleep_until(deadline);
    }
    return true;
}

} // namespace dvbstreamer5::media::testpattern
