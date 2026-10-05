from pathlib import Path


def replace_once(path, old, new):
    p = Path(path)
    text = p.read_text()
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{path}: expected one match, got {count}")
    p.write_text(text.replace(old, new, 1))

replace_once(
    "src/AppVersion.h",
    'inline constexpr const char* kProgramVersion = "10.8.100";',
    'inline constexpr const char* kProgramVersion = "10.8.101";')

replace_once(
    "src/media/NativeTranscoderPipeline.h",
    '''    using EncodedAudioObserver =
        std::function<void(const codec::EncodedAudioFrame&, std::uint64_t)>;''',
    '''    // V10.8.101: carry the actual elementary audio codec with shared ABR
    // audio.  When the primary output uses audio=copy there is no configured
    // encoder codec to infer from, so renditions must learn it from the source
    // PMT before muxing the shared compressed frames.
    using EncodedAudioObserver =
        std::function<void(mpegts::ElementaryCodec,
                           const codec::EncodedAudioFrame&, std::uint64_t)>;''')

replace_once(
    "src/media/NativeTranscoderPipeline.h",
    '''    bool pushEncodedAudioFrame(const codec::EncodedAudioFrame& frame,
                               std::uint64_t duration90k);''',
    '''    bool pushEncodedAudioFrame(mpegts::ElementaryCodec codec,
                               const codec::EncodedAudioFrame& frame,
                               std::uint64_t duration90k);''')

replace_once(
    "src/media/NativeTranscoderPipeline.cpp",
    '''bool NativeTranscoderPipeline::pushEncodedAudioFrame(
    const codec::EncodedAudioFrame& frame, std::uint64_t duration90k) {
    if (!externalAudioInput_.load(std::memory_order_acquire) ||
        failed_.load(std::memory_order_acquire)) return false;
    std::string localError;
    return emitAudio(frame, duration90k, localError);
}''',
    '''bool NativeTranscoderPipeline::pushEncodedAudioFrame(
    mpegts::ElementaryCodec codec,
    const codec::EncodedAudioFrame& frame, std::uint64_t duration90k) {
    if (!externalAudioInput_.load(std::memory_order_acquire) ||
        failed_.load(std::memory_order_acquire) ||
        codec == mpegts::ElementaryCodec::Unknown) return false;

    // Shared ABR audio bypasses this rendition's demux.  In audio=copy mode
    // initialize the rendition mux from the primary stream's real PMT codec;
    // otherwise the variant has no audio stream type and silently becomes
    // video-only even though compressed audio frames are being fanned out.
    std::string localError;
    {
        std::lock_guard<std::mutex> lock(muxMutex_);
        if (!mux_.setCodec(mpegts::ElementaryKind::Audio, codec, localError)) {
            setFailure(localError);
            return false;
        }
    }
    return emitAudio(frame, duration90k, localError);
}''')

replace_once(
    "src/media/NativeTranscoderPipeline.cpp",
    '''    if (observer) observer(frame, duration90k);''',
    '''    if (observer) {
        observer(audioCodecFromName(config_.audioCodec), frame, duration90k);
    }''')

replace_once(
    "src/media/NativeTranscoderPipeline.cpp",
    '''bool NativeTranscoderPipeline::emitCopy(mpegts::DemuxSample&& sample, std::string& error) {
    if (sample.stream.codec == mpegts::ElementaryCodec::Unknown) return true;
    {
        std::lock_guard<std::mutex> lock(muxMutex_);
        if (!mux_.setCodec(sample.stream.kind, sample.stream.codec, error)) return false;
    }
    MuxQueuedSample queued;''',
    '''bool NativeTranscoderPipeline::emitCopy(mpegts::DemuxSample&& sample, std::string& error) {
    if (sample.stream.codec == mpegts::ElementaryCodec::Unknown) return true;
    {
        std::lock_guard<std::mutex> lock(muxMutex_);
        if (!mux_.setCodec(sample.stream.kind, sample.stream.codec, error)) return false;
    }

    // V10.8.101: emitAudio() used to be the only source for the ABR shared
    // audio callback.  Therefore a perfectly valid primary audio=copy path
    // (for example DVB MP2) never delivered any audio to lower renditions.
    // Fan out the already-demuxed compressed audio before moving its payload
    // into the primary mux queue.  This code is dormant when ABR is disabled.
    if (sample.stream.kind == mpegts::ElementaryKind::Audio) {
        EncodedAudioObserver observer;
        {
            std::lock_guard<std::mutex> lock(encodedAudioObserverMutex_);
            observer = encodedAudioObserver_;
        }
        if (observer) {
            codec::EncodedAudioFrame frame;
            frame.data = sample.data;
            frame.pts90k = sample.pts90k;
            frame.hasPts = sample.hasPts;
            observer(sample.stream.codec, frame, 1920);
        }
    }

    MuxQueuedSample queued;''')

replace_once(
    "src/StreamManager.cpp",
    '''                state->nativeTranscoder->setEncodedAudioObserver(
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
                    });''',
    '''                state->nativeTranscoder->setEncodedAudioObserver(
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
                    });''')

replace_once(
    "src/StreamManager.cpp",
    '''    auto codecs = [&cfg]() {
        const std::string v = toLower(cfg.transcodeVideoCodec);
        const std::string a = toLower(cfg.transcodeAudioCodec);
        std::string c = (v == "hevc" || v == "h265") ? "hvc1.1.6.L120.B0" : "avc1.640028";
        if (a == "aac") c += ",mp4a.40.2";
        else if (a == "mp3") c += ",mp4a.6B";
        return c;
    }();''',
    '''    const std::string abrAudioCodec = toLower(cfg.transcodeAudioCodec);
    const bool advertiseCodecs = abrAudioCodec == "aac";
    auto codecs = [&cfg, &abrAudioCodec]() {
        const std::string v = toLower(cfg.transcodeVideoCodec);
        std::string c = (v == "hevc" || v == "h265") ? "hvc1.1.6.L120.B0" : "avc1.640028";
        if (abrAudioCodec == "aac") c += ",mp4a.40.2";
        return c;
    }();''')

replace_once(
    "src/StreamManager.cpp",
    '''        out << "#EXT-X-STREAM-INF:BANDWIDTH=" << bandwidth
            << ",AVERAGE-BANDWIDTH=" << std::min(bandwidth, average)
            << ",RESOLUTION=" << w << "x" << h
            << ",FRAME-RATE=25.000,CODECS=\\\"" << codecs << "\\\"\\n";
        out << uri << "\\n";''',
    '''        out << "#EXT-X-STREAM-INF:BANDWIDTH=" << bandwidth
            << ",AVERAGE-BANDWIDTH=" << std::min(bandwidth, average)
            << ",RESOLUTION=" << w << "x" << h
            << ",FRAME-RATE=25.000";
        // For audio=copy/MP2 the source codec is learned only from the live PMT.
        // Advertising a video-only CODECS list tells some HLS clients that the
        // rendition has no audio.  Omit the optional attribute unless the full
        // A/V codec list is known (AAC transcode); clients then probe the TS PMT.
        if (advertiseCodecs) out << ",CODECS=\\\"" << codecs << "\\\"";
        out << "\\n";
        out << uri << "\\n";''')

print("V10.8.101 patch applied")
