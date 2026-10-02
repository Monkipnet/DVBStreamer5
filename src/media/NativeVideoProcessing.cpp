#include "media/NativeVideoProcessing.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace dvbstreamer5::media::codec {
namespace {

struct BilinearMap {
    int sw = 0;
    int sh = 0;
    int dw = 0;
    int dh = 0;
    std::vector<int> x0;
    std::vector<int> x1;
    std::vector<std::uint16_t> wx;
    std::vector<int> y0;
    std::vector<int> y1;
    std::vector<std::uint16_t> wy;

    void prepare(int srcWidth, int srcHeight, int dstWidth, int dstHeight) {
        if (sw == srcWidth && sh == srcHeight &&
            dw == dstWidth && dh == dstHeight)
            return;

        sw = srcWidth;
        sh = srcHeight;
        dw = dstWidth;
        dh = dstHeight;

        x0.resize(static_cast<std::size_t>(dw));
        x1.resize(static_cast<std::size_t>(dw));
        wx.resize(static_cast<std::size_t>(dw));
        y0.resize(static_cast<std::size_t>(dh));
        y1.resize(static_cast<std::size_t>(dh));
        wy.resize(static_cast<std::size_t>(dh));

        // 8-bit fractional fixed point.  The old scaler recomputed floating
        // point coordinates, divisions and interpolation factors for every
        // pixel of every frame.  ABR at 1080/720/480/360 therefore spent most
        // CPU time in software resize even when encoding itself was VAAPI.
        for (int x = 0; x < dw; ++x) {
            const std::uint64_t fp = dw > 1
                ? (static_cast<std::uint64_t>(x) *
                   static_cast<std::uint64_t>(sw - 1) * 256ULL) /
                      static_cast<std::uint64_t>(dw - 1)
                : 0ULL;
            const int base = static_cast<int>(fp >> 8U);
            x0[static_cast<std::size_t>(x)] = base;
            x1[static_cast<std::size_t>(x)] = std::min(base + 1, sw - 1);
            wx[static_cast<std::size_t>(x)] =
                static_cast<std::uint16_t>(fp & 0xffU);
        }
        for (int y = 0; y < dh; ++y) {
            const std::uint64_t fp = dh > 1
                ? (static_cast<std::uint64_t>(y) *
                   static_cast<std::uint64_t>(sh - 1) * 256ULL) /
                      static_cast<std::uint64_t>(dh - 1)
                : 0ULL;
            const int base = static_cast<int>(fp >> 8U);
            y0[static_cast<std::size_t>(y)] = base;
            y1[static_cast<std::size_t>(y)] = std::min(base + 1, sh - 1);
            wy[static_cast<std::size_t>(y)] =
                static_cast<std::uint16_t>(fp & 0xffU);
        }
    }
};

void scalePlane(const std::uint8_t* src, int sw, int sh, int sstride,
                std::uint8_t* dst, int dw, int dh, int dstride) {
    if (sw == dw && sh == dh) {
        for (int y = 0; y < sh; ++y)
            std::memcpy(dst + y * dstride, src + y * sstride,
                        static_cast<std::size_t>(sw));
        return;
    }

    // Each transcoder worker keeps one geometry for long periods, so this
    // thread-local map removes all coordinate divisions from the hot frame
    // loop without requiring any global locks.
    thread_local BilinearMap map;
    map.prepare(sw, sh, dw, dh);

    for (int y = 0; y < dh; ++y) {
        const int y0 = map.y0[static_cast<std::size_t>(y)];
        const int y1 = map.y1[static_cast<std::size_t>(y)];
        const unsigned fy = map.wy[static_cast<std::size_t>(y)];
        const unsigned ify = 256U - fy;
        const auto* row0 = src + static_cast<std::size_t>(y0) * sstride;
        const auto* row1 = src + static_cast<std::size_t>(y1) * sstride;
        auto* out = dst + static_cast<std::size_t>(y) * dstride;

        for (int x = 0; x < dw; ++x) {
            const std::size_t ix = static_cast<std::size_t>(x);
            const int x0 = map.x0[ix];
            const int x1 = map.x1[ix];
            const unsigned fx = map.wx[ix];
            const unsigned ifx = 256U - fx;

            const unsigned top =
                static_cast<unsigned>(row0[x0]) * ifx +
                static_cast<unsigned>(row0[x1]) * fx;
            const unsigned bottom =
                static_cast<unsigned>(row1[x0]) * ifx +
                static_cast<unsigned>(row1[x1]) * fx;

            out[x] = static_cast<std::uint8_t>(
                (top * ify + bottom * fy + 32768U) >> 16U);
        }
    }
}

void deinterlacePlane(std::uint8_t* data, int w, int h, int stride) {
    if (h < 3) return;
    std::vector<std::uint8_t> row(static_cast<std::size_t>(w));
    for (int y = 1; y + 1 < h; y += 2) {
        for (int x = 0; x < w; ++x) {
            row[static_cast<std::size_t>(x)] = static_cast<std::uint8_t>(
                (static_cast<unsigned>(data[(y - 1) * stride + x]) + data[(y + 1) * stride + x] + 1U) / 2U);
        }
        std::memcpy(data + y * stride, row.data(), static_cast<std::size_t>(w));
    }
}

} // namespace

bool scaleI420(const RawVideoFrame& input, int width, int height,
               RawVideoFrame& output, std::string& error) {
    error.clear();
    if (input.width <= 0 || input.height <= 0 || width <= 0 || height <= 0 ||
        (input.width & 1) || (input.height & 1) || (width & 1) || (height & 1)) {
        error = "native I420 scaler requires positive even dimensions";
        return false;
    }
    const std::size_t expected = static_cast<std::size_t>(input.width) * input.height * 3U / 2U;
    if (input.i420.size() < expected) { error = "native I420 input frame is truncated"; return false; }
    output = {};
    output.width = width; output.height = height;
    output.pts90k = input.pts90k; output.dts90k = input.dts90k;
    output.hasPts = input.hasPts; output.hasDts = input.hasDts; output.keyFrame = input.keyFrame;
    output.i420.resize(static_cast<std::size_t>(width) * height * 3U / 2U);

    const auto* sy = input.i420.data();
    const auto* su = sy + static_cast<std::size_t>(input.width) * input.height;
    const auto* sv = su + static_cast<std::size_t>(input.width / 2) * (input.height / 2);
    auto* dy = output.i420.data();
    auto* du = dy + static_cast<std::size_t>(width) * height;
    auto* dv = du + static_cast<std::size_t>(width / 2) * (height / 2);
    scalePlane(sy, input.width, input.height, input.width, dy, width, height, width);
    scalePlane(su, input.width / 2, input.height / 2, input.width / 2, du, width / 2, height / 2, width / 2);
    scalePlane(sv, input.width / 2, input.height / 2, input.width / 2, dv, width / 2, height / 2, width / 2);
    return true;
}

void deinterlaceBlendI420(RawVideoFrame& frame) {
    if (frame.width <= 0 || frame.height <= 0 || frame.i420.empty()) return;
    auto* y = frame.i420.data();
    auto* u = y + static_cast<std::size_t>(frame.width) * frame.height;
    auto* v = u + static_cast<std::size_t>(frame.width / 2) * (frame.height / 2);
    deinterlacePlane(y, frame.width, frame.height, frame.width);
    deinterlacePlane(u, frame.width / 2, frame.height / 2, frame.width / 2);
    deinterlacePlane(v, frame.width / 2, frame.height / 2, frame.width / 2);
}

bool resamplePcm16(const PcmAudioFrame& input, int sampleRate, int channels,
                   PcmAudioFrame& output, std::string& error) {
    error.clear();
    if (input.sampleRate <= 0 || input.channels <= 0 || input.channels > 8 ||
        sampleRate <= 0 || channels <= 0 || channels > 8 ||
        input.samples.size() % static_cast<std::size_t>(input.channels) != 0) {
        error = "invalid native PCM resample parameters"; return false;
    }
    const std::size_t inFrames = input.samples.size() / static_cast<std::size_t>(input.channels);
    const std::size_t outFrames = inFrames == 0 ? 0 : static_cast<std::size_t>(
        std::llround(static_cast<double>(inFrames) * sampleRate / input.sampleRate));
    output = {}; output.sampleRate = sampleRate; output.channels = channels;
    output.pts90k = input.pts90k; output.hasPts = input.hasPts;
    output.samples.resize(outFrames * static_cast<std::size_t>(channels));
    auto sampleAt = [&](std::size_t frame, int ch) -> double {
        if (input.channels == channels) return input.samples[frame * input.channels + ch];
        if (channels == 1) {
            long sum = 0; for (int c = 0; c < input.channels; ++c) sum += input.samples[frame * input.channels + c];
            return static_cast<double>(sum) / input.channels;
        }
        if (input.channels == 1) return input.samples[frame];
        return input.samples[frame * input.channels + std::min(ch, input.channels - 1)];
    };
    for (std::size_t i = 0; i < outFrames; ++i) {
        const double pos = static_cast<double>(i) * input.sampleRate / sampleRate;
        const std::size_t a = std::min(static_cast<std::size_t>(pos), inFrames - 1);
        const std::size_t b = std::min(a + 1, inFrames - 1);
        const double f = pos - static_cast<double>(a);
        for (int c = 0; c < channels; ++c) {
            const double v = sampleAt(a, c) * (1.0 - f) + sampleAt(b, c) * f;
            output.samples[i * channels + c] = static_cast<std::int16_t>(std::clamp(v, -32768.0, 32767.0));
        }
    }
    return true;
}

} // namespace dvbstreamer5::media::codec
