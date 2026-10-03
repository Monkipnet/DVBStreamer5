from pathlib import Path


def replace(path, old, new, count=None):
    p = Path(path)
    s = p.read_text()
    hits = s.count(old)
    if hits == 0:
        raise SystemExit(f"pattern not found in {path}: {old[:100]!r}")
    if count is not None and hits != count:
        raise SystemExit(f"unexpected count in {path}: {hits} != {count} for {old[:80]!r}")
    p.write_text(s.replace(old, new))


replace('src/AppVersion.h', 'kProgramVersion = "10.8.39"', 'kProgramVersion = "10.8.40"', 1)

replace('CMakeLists.txt',
        '    src/media/NativeHardwareCodec.cpp\n    src/media/NativeNvidiaZeroCopy.cpp\n',
        '    src/media/NativeHardwareCodec.cpp\n    src/media/NativeIntelZeroCopy.cpp\n    src/media/NativeNvidiaZeroCopy.cpp\n', 1)

replace('src/media/NativeTranscoderPipeline.h',
        '#include "media/NativeCodecRuntime.h"\n#include "media/NativeNvidiaZeroCopy.h"\n',
        '#include "media/NativeCodecRuntime.h"\n#include "media/NativeIntelZeroCopy.h"\n#include "media/NativeNvidiaZeroCopy.h"\n', 1)

replace('src/media/NativeTranscoderPipeline.h',
        '    std::unique_ptr<codec::VideoDecoder> videoDecoder_;\n    std::unique_ptr<codec::NvidiaZeroCopyTranscoder> nvidiaZeroCopy_;\n',
        '    std::unique_ptr<codec::VideoDecoder> videoDecoder_;\n    std::unique_ptr<codec::IntelZeroCopyTranscoder> intelZeroCopy_;\n    bool intelZeroCopyAttempted_ = false;\n    bool intelZeroCopyFallbackLogged_ = false;\n    std::atomic<bool> intelZeroCopyActive_{false};\n    std::unique_ptr<codec::NvidiaZeroCopyTranscoder> nvidiaZeroCopy_;\n', 1)

replace('src/media/NativeTranscoderPipeline.cpp',
        '            nvidiaZeroCopy_.reset();\n            videoDecoder_.reset();\n            nvidiaZeroCopyAttempted_ = false;\n            nvidiaZeroCopyFallbackLogged_ = false;\n            nvidiaZeroCopyActive_.store(false, std::memory_order_release);\n',
        '            intelZeroCopy_.reset();\n            nvidiaZeroCopy_.reset();\n            videoDecoder_.reset();\n            intelZeroCopyAttempted_ = false;\n            intelZeroCopyFallbackLogged_ = false;\n            intelZeroCopyActive_.store(false, std::memory_order_release);\n            nvidiaZeroCopyAttempted_ = false;\n            nvidiaZeroCopyFallbackLogged_ = false;\n            nvidiaZeroCopyActive_.store(false, std::memory_order_release);\n', 1)

replace('src/media/NativeTranscoderPipeline.cpp',
        '                nvidiaZeroCopy_.reset();\n                videoDecoder_.reset();\n                nvidiaZeroCopyAttempted_ = false;\n                nvidiaZeroCopyFallbackLogged_ = false;\n                nvidiaZeroCopyActive_.store(false, std::memory_order_release);\n                videoStartupReady_ = false;\n',
        '                intelZeroCopy_.reset();\n                nvidiaZeroCopy_.reset();\n                videoDecoder_.reset();\n                intelZeroCopyAttempted_ = false;\n                intelZeroCopyFallbackLogged_ = false;\n                intelZeroCopyActive_.store(false, std::memory_order_release);\n                nvidiaZeroCopyAttempted_ = false;\n                nvidiaZeroCopyFallbackLogged_ = false;\n                nvidiaZeroCopyActive_.store(false, std::memory_order_release);\n                videoStartupReady_ = false;\n', 1)

replace('src/media/NativeTranscoderPipeline.cpp',
        '        nvidiaZeroCopy_.reset();\n        videoDecoder_.reset();\n        nvidiaZeroCopyAttempted_ = false;\n        nvidiaZeroCopyFallbackLogged_ = false;\n        nvidiaZeroCopyActive_.store(false, std::memory_order_release);\n',
        '        intelZeroCopy_.reset();\n        nvidiaZeroCopy_.reset();\n        videoDecoder_.reset();\n        intelZeroCopyAttempted_ = false;\n        intelZeroCopyFallbackLogged_ = false;\n        intelZeroCopyActive_.store(false, std::memory_order_release);\n        nvidiaZeroCopyAttempted_ = false;\n        nvidiaZeroCopyFallbackLogged_ = false;\n        nvidiaZeroCopyActive_.store(false, std::memory_order_release);\n', 1)

old_abr = '''    if (abrRawFanout && nvidiaZeroCopy_) {
        nvidiaZeroCopy_.reset();
        nvidiaZeroCopyActive_.store(false, std::memory_order_release);
        nvidiaZeroCopyAttempted_ = true;
        std::cerr << "NATIVE NVIDIA ZERO-COPY disabled reason=abr_shared_raw "
                     "fallback=shared_cpu_decode"
                  << std::endl;
    }
'''
new_abr = old_abr + '''    if (abrRawFanout && intelZeroCopy_) {
        intelZeroCopy_.reset();
        intelZeroCopyActive_.store(false, std::memory_order_release);
        intelZeroCopyAttempted_ = true;
        std::cerr << "NATIVE INTEL ZERO-COPY disabled reason=abr_shared_raw "
                     "fallback=shared_cpu_decode"
                  << std::endl;
    }
'''
replace('src/media/NativeTranscoderPipeline.cpp', old_abr, new_abr, 1)

intel_block = '''    const bool intelZeroCopyCodec =
        sample.stream.codec == mpegts::ElementaryCodec::H264 ||
        sample.stream.codec == mpegts::ElementaryCodec::H265 ||
        sample.stream.codec == mpegts::ElementaryCodec::Mpeg2Video;
    const bool intelZeroCopyBackend =
        config_.videoEncoder == "qsv" || config_.videoEncoder == "auto";
    if (!abrRawFanout && !nvidiaZeroCopy_ &&
        intelZeroCopyCodec && intelZeroCopyBackend) {
        if (!intelZeroCopyAttempted_) {
            intelZeroCopyAttempted_ = true;
            std::string zeroCopyError;
            if (codec::intelZeroCopyRuntimeAvailable()) {
                codec::IntelZeroCopyConfig zeroCopyConfig;
                zeroCopyConfig.inputCodec = sample.stream.codec;
                zeroCopyConfig.outputCodec = videoCodecFromName(config_.videoCodec);
                zeroCopyConfig.width = config_.width;
                zeroCopyConfig.height = config_.height;
                zeroCopyConfig.fps = config_.fps;
                zeroCopyConfig.bitrate = config_.videoBitrate;
                zeroCopyConfig.deinterlace = config_.deinterlace;
                intelZeroCopy_ = codec::createIntelZeroCopyTranscoder(
                    zeroCopyConfig, zeroCopyError);
            } else {
                zeroCopyError = "oneVPL hardware runtime API 2.1+ not available";
            }
            if (intelZeroCopy_) {
                intelZeroCopyActive_.store(true, std::memory_order_release);
                const char* inputName =
                    sample.stream.codec == mpegts::ElementaryCodec::H264 ? "h264" :
                    sample.stream.codec == mpegts::ElementaryCodec::H265 ? "hevc" : "mpeg2";
                std::cerr << "NATIVE INTEL ZERO-COPY selected input="
                          << inputName
                          << " output=" << config_.videoCodec
                          << " backend=" << config_.videoEncoder
                          << std::endl;
            } else if (!intelZeroCopyFallbackLogged_) {
                intelZeroCopyFallbackLogged_ = true;
                std::cerr << "NATIVE INTEL ZERO-COPY unavailable reason="
                          << zeroCopyError << " fallback="
                          << (config_.videoEncoder == "qsv"
                                  ? "cpu-decode+qsv" : "standard-auto-path")
                          << std::endl;
            }
        }

        if (intelZeroCopy_) {
            std::vector<codec::EncodedVideoFrame> encoded;
            std::string intelProcessError;
            if (!intelZeroCopy_->process(
                    sample.data.data(), sample.data.size(),
                    sample.pts90k, sample.hasPts, encoded, intelProcessError)) {
                std::cerr << "NATIVE INTEL ZERO-COPY runtime fallback reason="
                          << intelProcessError << " fallback="
                          << (config_.videoEncoder == "qsv"
                                  ? "cpu-decode+qsv" : "standard-auto-path")
                          << std::endl;
                intelZeroCopy_.reset();
                intelZeroCopyActive_.store(false, std::memory_order_release);
                intelZeroCopyFallbackLogged_ = true;
                error.clear();
            } else {
                decodedVideoFrames_ += encoded.size();
                if (!sourceGeometryResolved_ &&
                    intelZeroCopy_->sourceWidth() > 0 &&
                    intelZeroCopy_->sourceHeight() > 0) {
                    sourceGeometryResolved_ = true;
                    sourceWidth_ = intelZeroCopy_->sourceWidth();
                    sourceHeight_ = intelZeroCopy_->sourceHeight();
                    const int outWidth = intelZeroCopy_->outputWidth();
                    const int outHeight = intelZeroCopy_->outputHeight();
                    if (outWidth > 0 && outHeight > 0 &&
                        (outWidth != config_.width || outHeight != config_.height))
                        (void)setOutputGeometryIfUnconfigured(outWidth, outHeight);
                    std::cerr << "NATIVE VIDEO SOURCE GEOMETRY "
                              << sourceWidth_ << "x" << sourceHeight_
                              << " output=" << config_.width << "x" << config_.height
                              << " path=intel-zero-copy" << std::endl;
                }
                for (const auto& frame : encoded)
                    if (!emitVideo(frame, error)) return false;
                return true;
            }
        }
    }

'''
replace('src/media/NativeTranscoderPipeline.cpp',
        '    if (!ensureVideoDecoder(sample.stream.codec, error)) return false;\n',
        intel_block + '    if (!ensureVideoDecoder(sample.stream.codec, error)) return false;\n', 1)

old_flush = '''        if (nvidiaZeroCopy_) {
            std::vector<codec::EncodedVideoFrame> encoded;
            if (!nvidiaZeroCopy_->flush(encoded, error)) return false;
            for (const auto& frame : encoded)
                if (!emitVideo(frame, error)) return false;
        }
'''
new_flush = old_flush + '''        if (intelZeroCopy_) {
            std::vector<codec::EncodedVideoFrame> encoded;
            if (!intelZeroCopy_->flush(encoded, error)) return false;
            for (const auto& frame : encoded)
                if (!emitVideo(frame, error)) return false;
        }
'''
replace('src/media/NativeTranscoderPipeline.cpp', old_flush, new_flush, 1)

old_status = '''           " nvidia_zero_copy=" + std::to_string(
               nvidiaZeroCopyActive_.load(std::memory_order_acquire) ? 1 : 0) +
           " adrop=" + std::to_string(adrop);
'''
new_status = '''           " nvidia_zero_copy=" + std::to_string(
               nvidiaZeroCopyActive_.load(std::memory_order_acquire) ? 1 : 0) +
           " intel_zero_copy=" + std::to_string(
               intelZeroCopyActive_.load(std::memory_order_acquire) ? 1 : 0) +
           " adrop=" + std::to_string(adrop);
'''
replace('src/media/NativeTranscoderPipeline.cpp', old_status, new_status, 1)
