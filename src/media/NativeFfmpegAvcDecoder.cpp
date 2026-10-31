#include "media/NativeFfmpegAvcDecoder.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#if defined(DVBSTREAMER5_HAVE_FFMPEG_AVC)
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}
#endif

namespace dvbstreamer5::media::codec {
namespace {
#if defined(DVBSTREAMER5_HAVE_FFMPEG_AVC)

std::string avcError(int code) {
    char message[AV_ERROR_MAX_STRING_SIZE]{};
    if (av_strerror(code, message, sizeof(message)) == 0)
        return std::string(message);
    return "AVERROR " + std::to_string(code);
}

class FfmpegAvcDecoder final : public VideoDecoder {
public:
    ~FfmpegAvcDecoder() override {
        av_frame_free(&frame_);
        avcodec_free_context(&context_);
    }

    bool initialize(std::string& error) {
        error.clear();
        const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        if (!codec) {
            error = "FFmpeg H.264 decoder is unavailable in libavcodec";
            return false;
        }
        context_ = avcodec_alloc_context3(codec);
        if (!context_) {
            error = "FFmpeg H.264 decoder AVCodecContext allocation failed";
            return false;
        }

        // One decoder worker keeps interlaced field ordering deterministic.
        // The existing Ittiam/OpenH264 decoders and the encoder choice are
        // unchanged; this backend is only selected explicitly per stream.
        context_->thread_count = 1;
        context_->thread_type = 0;
        context_->pkt_timebase = AVRational{1, 90000};
        context_->time_base = AVRational{1, 90000};
        const int opened = avcodec_open2(context_, codec, nullptr);
        if (opened < 0) {
            error = "FFmpeg H.264 decoder open failed: " + avcError(opened);
            return false;
        }
        frame_ = av_frame_alloc();
        if (!frame_) {
            error = "FFmpeg H.264 decoder AVFrame allocation failed";
            return false;
        }
        std::cerr << "NATIVE AVC DECODER backend=ffmpeg-libavcodec"
                  << " threads=1 interlaced=1 output=i420"
                  << " codec_version=" << avcodec_version()
                  << std::endl;
        return true;
    }

    bool decode(const std::uint8_t* data, std::size_t size,
                std::uint64_t pts90k, bool hasPts,
                std::vector<RawVideoFrame>& output,
                std::string& error) override {
        error.clear();
        if (!context_ || !frame_) {
            error = "FFmpeg H.264 decoder not initialized";
            return false;
        }
        if (!data || size == 0) return true;
        if (size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            error = "FFmpeg H.264 access unit exceeds signed packet limit";
            return false;
        }

        AVPacket* packet = av_packet_alloc();
        if (!packet) {
            error = "FFmpeg H.264 packet allocation failed";
            return false;
        }
        const int alloc = av_new_packet(packet, static_cast<int>(size));
        if (alloc < 0) {
            error = "FFmpeg H.264 packet allocation failed: " + avcError(alloc);
            av_packet_free(&packet);
            return false;
        }
        std::memcpy(packet->data, data, size);
        packet->pts = hasPts ? static_cast<std::int64_t>(pts90k)
                             : AV_NOPTS_VALUE;
        packet->dts = AV_NOPTS_VALUE;

        // A full decoder queue can request receive-before-send. Drain once
        // and retry the *same* AU; never drop it or feed it twice.
        int status = avcodec_send_packet(context_, packet);
        if (status == AVERROR(EAGAIN)) {
            if (!receiveFrames(output, pts90k, hasPts, error)) {
                av_packet_free(&packet);
                return false;
            }
            status = avcodec_send_packet(context_, packet);
        }
        av_packet_free(&packet);

        if (status == AVERROR_INVALIDDATA) {
            logCorruptInput(status);
            return true;
        }
        if (status < 0) {
            error = "FFmpeg H.264 send_packet failed: " + avcError(status);
            return false;
        }
        return receiveFrames(output, pts90k, hasPts, error);
    }

    void reset() override {
        if (context_) avcodec_flush_buffers(context_);
        frames_ = 0;
        invalidPackets_ = 0;
    }

private:
    bool receiveFrames(std::vector<RawVideoFrame>& output,
                       std::uint64_t pts90k, bool hasPts,
                       std::string& error) {
        while (true) {
            const int status = avcodec_receive_frame(context_, frame_);
            if (status == AVERROR(EAGAIN) || status == AVERROR_EOF)
                return true;
            if (status == AVERROR_INVALIDDATA) {
                logCorruptInput(status);
                return true;
            }
            if (status < 0) {
                error = "FFmpeg H.264 receive_frame failed: " + avcError(status);
                return false;
            }
            RawVideoFrame raw;
            const bool copied = copyFrame(*frame_, pts90k, hasPts, raw, error);
            av_frame_unref(frame_);
            if (!copied) return false;
            output.push_back(std::move(raw));
        }
    }

    bool copyFrame(const AVFrame& src, std::uint64_t fallbackPts,
                   bool fallbackHasPts, RawVideoFrame& dest,
                   std::string& error) {
        const int format = src.format;
        if ((format != AV_PIX_FMT_YUV420P && format != AV_PIX_FMT_YUVJ420P) ||
            src.width <= 0 || src.height <= 0 ||
            (src.width & 1) || (src.height & 1) ||
            src.width > 8192 || src.height > 4320 ||
            !src.data[0] || !src.data[1] || !src.data[2] ||
            src.linesize[0] < src.width ||
            src.linesize[1] < src.width / 2 ||
            src.linesize[2] < src.width / 2) {
            error = "FFmpeg H.264 decoder returned unsupported pixel format "
                    "or invalid I420 frame: format=" +
                    std::to_string(format) +
                    " size=" + std::to_string(src.width) +
                    "x" + std::to_string(src.height);
            return false;
        }

        dest.width = src.width;
        dest.height = src.height;
        const std::size_t lumaSize =
            static_cast<std::size_t>(src.width) * src.height;
        dest.i420.resize(lumaSize + lumaSize / 2);
        auto* y = dest.i420.data();
        auto* u = y + lumaSize;
        auto* v = u + lumaSize / 4;
        for (int row = 0; row < src.height; ++row)
            std::memcpy(y + static_cast<std::size_t>(row) * src.width,
                        src.data[0] + static_cast<std::size_t>(row) * src.linesize[0],
                        static_cast<std::size_t>(src.width));
        for (int row = 0; row < src.height / 2; ++row) {
            std::memcpy(u + static_cast<std::size_t>(row) * (src.width / 2),
                        src.data[1] + static_cast<std::size_t>(row) * src.linesize[1],
                        static_cast<std::size_t>(src.width / 2));
            std::memcpy(v + static_cast<std::size_t>(row) * (src.width / 2),
                        src.data[2] + static_cast<std::size_t>(row) * src.linesize[2],
                        static_cast<std::size_t>(src.width / 2));
        }

        const std::int64_t bestPts = src.best_effort_timestamp;
        dest.hasPts = bestPts != AV_NOPTS_VALUE || fallbackHasPts;
        dest.pts90k = bestPts != AV_NOPTS_VALUE
            ? static_cast<std::uint64_t>(bestPts) : fallbackPts;
        dest.hasDts = dest.hasPts;
        dest.dts90k = dest.pts90k;
#if defined(AV_FRAME_FLAG_KEY)
        dest.keyFrame = (src.flags & AV_FRAME_FLAG_KEY) != 0;
#else
        dest.keyFrame = src.key_frame != 0;
#endif
#if defined(AV_FRAME_FLAG_INTERLACED)
        dest.interlaced = (src.flags & AV_FRAME_FLAG_INTERLACED) != 0;
#else
        dest.interlaced = src.interlaced_frame != 0;
#endif
        ++frames_;
        if (frames_ <= 4 || (frames_ % 250U) == 0U) {
            std::cerr << "NATIVE AVC FFMPEG OUTPUT frames=" << frames_
                      << " size=" << dest.width << "x" << dest.height
                      << " interlaced=" << (dest.interlaced ? 1 : 0)
                      << " pts=" << (dest.hasPts
                            ? std::to_string(dest.pts90k) : std::string("-"))
                      << std::endl;
        }
        return true;
    }

    void logCorruptInput(int code) {
        ++invalidPackets_;
        if (invalidPackets_ <= 4 || invalidPackets_ % 100U == 0U)
            std::cerr << "NATIVE AVC FFMPEG recoverable"
                      << " packets=" << invalidPackets_
                      << " error=" << avcError(code) << std::endl;
    }

    AVCodecContext* context_ = nullptr;
    AVFrame* frame_ = nullptr;
    std::uint64_t frames_ = 0;
    std::uint64_t invalidPackets_ = 0;
};

#endif
} // namespace

bool ffmpegAvcDecoderAvailable() noexcept {
#if defined(DVBSTREAMER5_HAVE_FFMPEG_AVC)
    return avcodec_find_decoder(AV_CODEC_ID_H264) != nullptr;
#else
    return false;
#endif
}

std::unique_ptr<VideoDecoder> createFfmpegAvcDecoder(std::string& error) {
#if defined(DVBSTREAMER5_HAVE_FFMPEG_AVC)
    auto decoder = std::make_unique<FfmpegAvcDecoder>();
    if (!decoder->initialize(error)) return {};
    return decoder;
#else
    error = "FFmpeg H.264 decoder was not compiled in. "
            "Install libavcodec-dev libavutil-dev and rebuild.";
    return {};
#endif
}

} // namespace dvbstreamer5::media::codec
