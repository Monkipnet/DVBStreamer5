#include "media/NativeVideoProcessing.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace dvbstreamer5::media::codec {
namespace {

void scalePlane(const std::uint8_t* src, int sw, int sh, int sstride,
                std::uint8_t* dst, int dw, int dh, int dstride) {
    if (sw == dw && sh == dh) {
        for (int y = 0; y < sh; ++y) std::memcpy(dst + y * dstride, src + y * sstride, static_cast<std::size_t>(sw));
        return;
    }
    // Bilinear scaler with integer fixed-point coordinates. It is deliberately
    // small and dependency-free; SIMD specializations can replace this later.
    for (int y = 0; y < dh; ++y) {
        const double sy = dh > 1 ? (static_cast<double>(y) * (sh - 1)) / (dh - 1) : 0.0;
        const int y0 = static_cast<int>(sy);
        const int y1 = std::min(y0 + 1, sh - 1);
        const double fy = sy - y0;
        for (int x = 0; x < dw; ++x) {
            const double sx = dw > 1 ? (static_cast<double>(x) * (sw - 1)) / (dw - 1) : 0.0;
            const int x0 = static_cast<int>(sx);
            const int x1 = std::min(x0 + 1, sw - 1);
            const double fx = sx - x0;
            const double a = src[y0 * sstride + x0] * (1.0 - fx) + src[y0 * sstride + x1] * fx;
            const double b = src[y1 * sstride + x0] * (1.0 - fx) + src[y1 * sstride + x1] * fx;
            dst[y * dstride + x] = static_cast<std::uint8_t>(std::clamp(a * (1.0 - fy) + b * fy, 0.0, 255.0) + 0.5);
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
