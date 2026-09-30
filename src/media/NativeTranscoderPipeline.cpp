#include "media/NativeTranscoderPipeline.h"

#include "media/NativeVideoProcessing.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace dvbstreamer5::media::transcode {
namespace {

mpegts::ElementaryCodec videoCodecFromName(const std::string& codec) {
    if (codec == "h264") return mpegts::ElementaryCodec::H264;
    if (codec == "hevc" || codec == "h265") return mpegts::ElementaryCodec::H265;
    return mpegts::ElementaryCodec::Unknown;
}

mpegts::ElementaryCodec audioCodecFromName(const std::string& codec) {
    if (codec == "aac") return mpegts::ElementaryCodec::AacAdts;
    if (codec == "mp2" || codec == "mp3") return mpegts::ElementaryCodec::MpegAudio;
    return mpegts::ElementaryCodec::Unknown;
}

std::uint64_t videoDuration90k(double fps) {
    if (fps < 1.0) fps = 25.0;
    return static_cast<std::uint64_t>(std::llround(90000.0 / fps));
}

} // namespace

NativeTranscoderPipeline::NativeTranscoderPipeline() = default;
NativeTranscoderPipeline::~NativeTranscoderPipeline() = default;

bool NativeTranscoderPipeline::initialize(const NativeTranscoderConfig& config, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    error.clear();
    reset();
    config_ = config;
    if (config_.videoCodec != "copy" && videoCodecFromName(config_.videoCodec) == mpegts::ElementaryCodec::Unknown) {
        error = "native transcoder: unsupported video output codec";
        return false;
    }
    if (config_.audioCodec != "copy" && config_.audioCodec != "aac" && config_.audioCodec != "mp2") {
        error = "native transcoder: supported audio output codecs are copy, aac and mp2";
        return false;
    }
    if (config_.videoCodec != "copy" && ((config_.width & 1) || (config_.height & 1) || config_.width <= 0 || config_.height <= 0)) {
        error = "native transcoder: video resolution must use positive even dimensions";
        return false;
    }

    mpegts::NativeMuxConfig muxConfig;
    muxConfig.serviceId = config_.serviceId ? config_.serviceId : 1;
    muxConfig.videoPid = config_.videoPid ? config_.videoPid : 0x0100;
    muxConfig.audioPid = config_.audioPid ? config_.audioPid : 0x0101;
    muxConfig.targetBitrate = config_.muxBitrate;
    muxConfig.serviceName = config_.serviceName.empty() ? "DVBStreamer5" : config_.serviceName;
    muxConfig.serviceProvider = config_.serviceProvider.empty() ? "DVBStreamer5" : config_.serviceProvider;
    if (!mux_.initialize(muxConfig, error)) return false;
    if (config_.videoCodec != "copy" && !mux_.setCodec(mpegts::ElementaryKind::Video, videoCodecFromName(config_.videoCodec), error)) return false;
    if (config_.audioCodec != "copy" && !mux_.setCodec(mpegts::ElementaryKind::Audio, audioCodecFromName(config_.audioCodec), error)) return false;

    demux_.setProgramCallback([this](const std::vector<mpegts::DemuxStreamInfo>& streams) { onProgram(streams); });
    demux_.setSampleCallback([this](mpegts::DemuxSample&& sample) { onSample(std::move(sample)); });
    initialized_ = true;
    return true;
}

void NativeTranscoderPipeline::reset() {
    demux_.reset();
    mux_.reset();
    videoDecoder_.reset();
    videoEncoder_.reset();
    audioDecoder_.reset();
    aacEncoder_.reset();
    mp2Encoder_.reset();
    inputVideoCodec_ = mpegts::ElementaryCodec::Unknown;
    inputAudioCodec_ = mpegts::ElementaryCodec::Unknown;
    videoEncoderConfigured_ = false;
    audioEncoderConfigured_ = false;
    initialized_ = false;
    failed_ = false;
    failure_.clear();
    pendingOutput_.clear();
}

void NativeTranscoderPipeline::onProgram(const std::vector<mpegts::DemuxStreamInfo>& streams) {
    for (const auto& stream : streams) {
        if (stream.kind == mpegts::ElementaryKind::Video && inputVideoCodec_ == mpegts::ElementaryCodec::Unknown)
            inputVideoCodec_ = stream.codec;
        if (stream.kind == mpegts::ElementaryKind::Audio && inputAudioCodec_ == mpegts::ElementaryCodec::Unknown)
            inputAudioCodec_ = stream.codec;
    }
}

void NativeTranscoderPipeline::onSample(mpegts::DemuxSample&& sample) {
    if (failed_) return;
    std::string error;
    bool ok = sample.stream.kind == mpegts::ElementaryKind::Video
        ? handleVideo(std::move(sample), error)
        : handleAudio(std::move(sample), error);
    if (!ok) {
        failed_ = true;
        failure_ = error.empty() ? "native transcoder sample processing failed" : error;
    }
}

bool NativeTranscoderPipeline::ensureVideoDecoder(mpegts::ElementaryCodec codecType, std::string& error) {
    if (videoDecoder_ && inputVideoCodec_ == codecType) return true;
    videoDecoder_.reset();
    inputVideoCodec_ = codecType;
    videoDecoder_ = codec::createVideoDecoder(codecType, error);
    return static_cast<bool>(videoDecoder_);
}

bool NativeTranscoderPipeline::ensureVideoEncoder(std::string& error) {
    if (videoEncoderConfigured_ && videoEncoder_) return true;
    videoEncoder_ = codec::createVideoEncoder(videoCodecFromName(config_.videoCodec), error);
    if (!videoEncoder_) return false;
    if (!videoEncoder_->configure(config_.width, config_.height, config_.fps,
                                  config_.videoBitrate, error)) return false;
    videoEncoderConfigured_ = true;
    return true;
}

bool NativeTranscoderPipeline::ensureAudioDecoder(mpegts::ElementaryCodec codecType, std::string& error) {
    if (audioDecoder_ && inputAudioCodec_ == codecType) return true;
    audioDecoder_.reset();
    inputAudioCodec_ = codecType;
    audioDecoder_ = codec::createAudioDecoder(codecType, error);
    return static_cast<bool>(audioDecoder_);
}

bool NativeTranscoderPipeline::ensureAudioEncoder(const codec::PcmAudioFrame&, std::string& error) {
    if (audioEncoderConfigured_) return true;
    if (config_.audioCodec == "aac") {
        aacEncoder_ = codec::createAacEncoder(error);
        if (!aacEncoder_) return false;
        if (!aacEncoder_->configure(48000, 2, config_.audioBitrate, error)) return false;
    } else if (config_.audioCodec == "mp2") {
        mp2Encoder_ = std::make_unique<Mp2Encoder>();
        Mp2EncoderConfig cfg;
        cfg.sampleRate = 48000;
        cfg.channels = 2;
        cfg.bitrate = static_cast<std::uint32_t>(config_.audioBitrate);
        if (!mp2Encoder_->initialize(cfg, error)) return false;
    } else {
        error = "native audio encoder is not configured";
        return false;
    }
    audioEncoderConfigured_ = true;
    return true;
}

bool NativeTranscoderPipeline::handleVideo(mpegts::DemuxSample&& sample, std::string& error) {
    if (config_.videoCodec == "copy") return emitCopy(std::move(sample), error);
    if (!ensureVideoDecoder(sample.stream.codec, error) || !ensureVideoEncoder(error)) return false;
    std::vector<codec::RawVideoFrame> decoded;
    if (!videoDecoder_->decode(sample.data.data(), sample.data.size(), sample.pts90k, sample.hasPts, decoded, error)) return false;
    for (auto& raw : decoded) {
        if (config_.deinterlace) codec::deinterlaceBlendI420(raw);
        codec::RawVideoFrame scaled;
        const codec::RawVideoFrame* source = &raw;
        if (raw.width != config_.width || raw.height != config_.height) {
            if (!codec::scaleI420(raw, config_.width, config_.height, scaled, error)) return false;
            source = &scaled;
        }
        std::vector<codec::EncodedVideoFrame> encoded;
        if (!videoEncoder_->encode(*source, encoded, error)) return false;
        for (const auto& frame : encoded) if (!emitVideo(frame, error)) return false;
    }
    return true;
}

bool NativeTranscoderPipeline::handleAudio(mpegts::DemuxSample&& sample, std::string& error) {
    if (config_.audioCodec == "copy") return emitCopy(std::move(sample), error);
    if (!ensureAudioDecoder(sample.stream.codec, error)) return false;
    std::vector<codec::PcmAudioFrame> decoded;
    if (!audioDecoder_->decode(sample.data.data(), sample.data.size(), sample.pts90k, sample.hasPts, decoded, error)) return false;
    for (const auto& pcm : decoded) {
        if (!ensureAudioEncoder(pcm, error)) return false;
        codec::PcmAudioFrame normalized;
        if (!codec::resamplePcm16(pcm, 48000, 2, normalized, error)) return false;
        if (config_.audioCodec == "aac") {
            std::vector<codec::EncodedAudioFrame> encoded;
            if (!aacEncoder_->encode(normalized, encoded, error)) return false;
            for (const auto& frame : encoded) if (!emitAudio(frame, 1920, error)) return false;
        } else {
            const std::size_t frames = normalized.samples.size() / 2U;
            std::vector<std::uint8_t> bytes;
            if (!mp2Encoder_->encodeInterleaved(normalized.samples.data(), frames, bytes, error)) return false;
            if (!bytes.empty()) {
                codec::EncodedAudioFrame frame;
                frame.data = std::move(bytes);
                frame.hasPts = normalized.hasPts;
                frame.pts90k = normalized.pts90k;
                if (!emitAudio(frame, 2160, error)) return false;
            }
        }
    }
    return true;
}

bool NativeTranscoderPipeline::emitVideo(const codec::EncodedVideoFrame& frame, std::string& error) {
    mpegts::ElementarySample sample;
    sample.data = frame.data.data(); sample.size = frame.data.size();
    sample.pts90k = frame.pts90k; sample.dts90k = frame.dts90k;
    sample.duration90k = videoDuration90k(config_.fps);
    sample.hasPts = frame.hasPts; sample.hasDts = frame.hasDts; sample.randomAccess = frame.keyFrame;
    std::vector<mpegts::Packet> packets;
    if (!mux_.write(mpegts::ElementaryKind::Video, sample, packets, error)) return false;
    appendPackets(packets); return true;
}

bool NativeTranscoderPipeline::emitAudio(const codec::EncodedAudioFrame& frame,
                                         std::uint64_t duration90k, std::string& error) {
    mpegts::ElementarySample sample;
    sample.data = frame.data.data(); sample.size = frame.data.size();
    sample.pts90k = frame.pts90k; sample.dts90k = frame.pts90k; sample.duration90k = duration90k;
    sample.hasPts = frame.hasPts; sample.hasDts = frame.hasPts;
    std::vector<mpegts::Packet> packets;
    if (!mux_.write(mpegts::ElementaryKind::Audio, sample, packets, error)) return false;
    appendPackets(packets); return true;
}

bool NativeTranscoderPipeline::emitCopy(mpegts::DemuxSample&& sample, std::string& error) {
    if (sample.stream.codec == mpegts::ElementaryCodec::Unknown) return true;
    if (!mux_.setCodec(sample.stream.kind, sample.stream.codec, error)) return false;
    mpegts::ElementarySample es;
    es.data = sample.data.data(); es.size = sample.data.size(); es.pts90k = sample.pts90k; es.dts90k = sample.dts90k;
    es.hasPts = sample.hasPts; es.hasDts = sample.hasDts; es.randomAccess = sample.randomAccess;
    es.duration90k = sample.stream.kind == mpegts::ElementaryKind::Video ? videoDuration90k(config_.fps) : 1920;
    std::vector<mpegts::Packet> packets;
    if (!mux_.write(sample.stream.kind, es, packets, error)) return false;
    appendPackets(packets); return true;
}

void NativeTranscoderPipeline::appendPackets(const std::vector<mpegts::Packet>& packets) {
    const std::size_t old = pendingOutput_.size();
    pendingOutput_.resize(old + packets.size() * mpegts::kPacketSize);
    for (std::size_t i = 0; i < packets.size(); ++i)
        std::copy(packets[i].begin(), packets[i].end(), pendingOutput_.begin() + static_cast<std::ptrdiff_t>(old + i * mpegts::kPacketSize));
}

bool NativeTranscoderPipeline::process(const std::uint8_t* data, std::size_t size,
                                       std::vector<std::uint8_t>& output, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    error.clear(); output.clear();
    if (!initialized_) { error = "native transcoder is not initialized"; return false; }
    if (failed_) { error = failure_; return false; }
    pendingOutput_.clear();
    if (!demux_.push(data, size, error)) return false;
    if (failed_) { error = failure_; return false; }
    output.swap(pendingOutput_);
    return true;
}

bool NativeTranscoderPipeline::flush(std::vector<std::uint8_t>& output, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    error.clear(); output.clear(); pendingOutput_.clear();
    if (!initialized_) return true;
    demux_.flush();
    if (failed_) { error = failure_; return false; }
    if (videoEncoder_) {
        std::vector<codec::EncodedVideoFrame> encoded;
        if (!videoEncoder_->flush(encoded, error)) return false;
        for (const auto& frame : encoded) if (!emitVideo(frame, error)) return false;
    }
    if (aacEncoder_) {
        std::vector<codec::EncodedAudioFrame> encoded;
        if (!aacEncoder_->flush(encoded, error)) return false;
        for (const auto& frame : encoded) if (!emitAudio(frame, 1920, error)) return false;
    }
    if (mp2Encoder_) {
        std::vector<std::uint8_t> bytes;
        if (!mp2Encoder_->finish(bytes, error)) return false;
        if (!bytes.empty()) {
            codec::EncodedAudioFrame frame; frame.data = std::move(bytes);
            if (!emitAudio(frame, 2160, error)) return false;
        }
    }
    output.swap(pendingOutput_);
    return true;
}

std::string NativeTranscoderPipeline::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (failed_) return failure_;
    if (!initialized_) return "not initialized";
    return "running";
}

} // namespace dvbstreamer5::media::transcode
