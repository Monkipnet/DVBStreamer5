#include "media/NativeVideoProcessing.h"
#include "media/NativeScaleMode.h"
#include "media/NativeScalerHorizontal.h"
#include "media/NativeScalerVertical.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>

#if defined(DVBSTREAMER5_HAVE_VAAPI)
#include <fcntl.h>
#include <unistd.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_vpp.h>
#endif

namespace dvbstreamer5::media::codec {
namespace {

#if defined(DVBSTREAMER5_HAVE_VAAPI)
class VaapiVppScaler {
public:
    ~VaapiVppScaler() { close(); }

    bool scale(const RawVideoFrame& input, int width, int height,
               RawVideoFrame& output) {
        if (!ensure(input.width, input.height, width, height)) return false;
        if (!upload(input)) return false;

        VARectangle src{};
        src.x = 0;
        src.y = 0;
        src.width = static_cast<unsigned short>(input.width);
        src.height = static_cast<unsigned short>(input.height);

        VARectangle dst{};
        dst.x = 0;
        dst.y = 0;
        dst.width = static_cast<unsigned short>(width);
        dst.height = static_cast<unsigned short>(height);

        VAProcPipelineParameterBuffer param{};
        param.surface = inputSurface_;
        param.surface_region = &src;
        param.output_region = &dst;
        param.filter_flags = VA_FILTER_SCALING_DEFAULT;

        VABufferID pipeline = VA_INVALID_ID;
        VAStatus st = vaCreateBuffer(
            display_, context_, VAProcPipelineParameterBufferType,
            sizeof(param), 1, &param, &pipeline);
        if (st != VA_STATUS_SUCCESS) return fail();

        st = vaBeginPicture(display_, context_, outputSurface_);
        if (st == VA_STATUS_SUCCESS)
            st = vaRenderPicture(display_, context_, &pipeline, 1);
        if (st == VA_STATUS_SUCCESS)
            st = vaEndPicture(display_, context_);
        vaDestroyBuffer(display_, pipeline);
        if (st != VA_STATUS_SUCCESS) return fail();

        st = vaSyncSurface(display_, outputSurface_);
        if (st != VA_STATUS_SUCCESS) return fail();

        if (!download(output)) return false;
        output.pts90k = input.pts90k;
        output.dts90k = input.dts90k;
        output.hasPts = input.hasPts;
        output.hasDts = input.hasDts;
        output.keyFrame = input.keyFrame;

        if (!logged_) {
            std::cerr << "NATIVE VIDEO SCALE backend=vaapi-vpp src="
                      << input.width << "x" << input.height
                      << " dst=" << width << "x" << height
                      << std::endl;
            logged_ = true;
        }
        return true;
    }

private:
    bool ensure(int sw, int sh, int dw, int dh) {
        if (ready_ && sw_ == sw && sh_ == sh && dw_ == dw && dh_ == dh)
            return true;
        close();

        for (int i = 128; i < 160 && fd_ < 0; ++i) {
            const std::string path =
                "/dev/dri/renderD" + std::to_string(i);
            fd_ = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
        }
        if (fd_ < 0) return false;

        display_ = vaGetDisplayDRM(fd_);
        if (!display_) return fail();

        int major = 0, minor = 0;
        if (vaInitialize(display_, &major, &minor) != VA_STATUS_SUCCESS)
            return fail();

        if (vaCreateConfig(
                display_, VAProfileNone, VAEntrypointVideoProc,
                nullptr, 0, &config_) != VA_STATUS_SUCCESS)
            return fail();

        VASurfaceAttrib attr{};
        attr.type = VASurfaceAttribPixelFormat;
        attr.flags = VA_SURFACE_ATTRIB_SETTABLE;
        attr.value.type = VAGenericValueTypeInteger;
        attr.value.value.i = VA_FOURCC_NV12;

        if (vaCreateSurfaces(
                display_, VA_RT_FORMAT_YUV420,
                static_cast<unsigned>(sw),
                static_cast<unsigned>(sh),
                &inputSurface_, 1, &attr, 1) != VA_STATUS_SUCCESS)
            return fail();

        if (vaCreateSurfaces(
                display_, VA_RT_FORMAT_YUV420,
                static_cast<unsigned>(dw),
                static_cast<unsigned>(dh),
                &outputSurface_, 1, &attr, 1) != VA_STATUS_SUCCESS)
            return fail();

        VASurfaceID renderTarget = outputSurface_;
        if (vaCreateContext(
                display_, config_, dw, dh, VA_PROGRESSIVE,
                &renderTarget, 1, &context_) != VA_STATUS_SUCCESS)
            return fail();

        sw_ = sw;
        sh_ = sh;
        dw_ = dw;
        dh_ = dh;
        ready_ = true;
        return true;
    }

    bool upload(const RawVideoFrame& input) {
        VAImage image{};
        if (vaDeriveImage(display_, inputSurface_, &image) != VA_STATUS_SUCCESS)
            return fail();
        void* mapped = nullptr;
        if (vaMapBuffer(display_, image.buf, &mapped) != VA_STATUS_SUCCESS) {
            vaDestroyImage(display_, image.image_id);
            return fail();
        }

        bool ok = image.format.fourcc == VA_FOURCC_NV12;
        if (ok) {
            auto* base = static_cast<std::uint8_t*>(mapped);
            const auto* srcY = input.i420.data();
            const auto* srcU =
                srcY + static_cast<std::size_t>(sw_) * sh_;
            const auto* srcV =
                srcU + static_cast<std::size_t>(sw_ / 2) * (sh_ / 2);

            for (int y = 0; y < sh_; ++y)
                std::memcpy(
                    base + image.offsets[0] +
                        static_cast<std::size_t>(y) * image.pitches[0],
                    srcY + static_cast<std::size_t>(y) * sw_,
                    static_cast<std::size_t>(sw_));

            for (int y = 0; y < sh_ / 2; ++y) {
                auto* dst =
                    base + image.offsets[1] +
                    static_cast<std::size_t>(y) * image.pitches[1];
                const auto* u =
                    srcU + static_cast<std::size_t>(y) * (sw_ / 2);
                const auto* v =
                    srcV + static_cast<std::size_t>(y) * (sw_ / 2);
                for (int x = 0; x < sw_ / 2; ++x) {
                    dst[x * 2] = u[x];
                    dst[x * 2 + 1] = v[x];
                }
            }
        }

        vaUnmapBuffer(display_, image.buf);
        vaDestroyImage(display_, image.image_id);
        return ok;
    }

    bool download(RawVideoFrame& output) {
        VAImage image{};
        if (vaDeriveImage(display_, outputSurface_, &image) != VA_STATUS_SUCCESS)
            return fail();
        void* mapped = nullptr;
        if (vaMapBuffer(display_, image.buf, &mapped) != VA_STATUS_SUCCESS) {
            vaDestroyImage(display_, image.image_id);
            return fail();
        }

        bool ok = image.format.fourcc == VA_FOURCC_NV12;
        if (ok) {
            output = {};
            output.width = dw_;
            output.height = dh_;
            output.i420.resize(
                static_cast<std::size_t>(dw_) * dh_ * 3U / 2U);

            const auto* base = static_cast<const std::uint8_t*>(mapped);
            auto* dstY = output.i420.data();
            auto* dstU =
                dstY + static_cast<std::size_t>(dw_) * dh_;
            auto* dstV =
                dstU + static_cast<std::size_t>(dw_ / 2) * (dh_ / 2);

            for (int y = 0; y < dh_; ++y)
                std::memcpy(
                    dstY + static_cast<std::size_t>(y) * dw_,
                    base + image.offsets[0] +
                        static_cast<std::size_t>(y) * image.pitches[0],
                    static_cast<std::size_t>(dw_));

            for (int y = 0; y < dh_ / 2; ++y) {
                const auto* src =
                    base + image.offsets[1] +
                    static_cast<std::size_t>(y) * image.pitches[1];
                auto* u =
                    dstU + static_cast<std::size_t>(y) * (dw_ / 2);
                auto* v =
                    dstV + static_cast<std::size_t>(y) * (dw_ / 2);
                for (int x = 0; x < dw_ / 2; ++x) {
                    u[x] = src[x * 2];
                    v[x] = src[x * 2 + 1];
                }
            }
        }

        vaUnmapBuffer(display_, image.buf);
        vaDestroyImage(display_, image.image_id);
        return ok;
    }

    bool fail() {
        if (!fallbackLogged_) {
            std::cerr << "NATIVE VIDEO SCALE vaapi-vpp unavailable; "
                         "falling back to fixed-point CPU scaler"
                      << std::endl;
            fallbackLogged_ = true;
        }
        return false;
    }

    void close() {
        ready_ = false;
        if (display_ && context_ != VA_INVALID_ID)
            vaDestroyContext(display_, context_);
        context_ = VA_INVALID_ID;

        if (display_ && inputSurface_ != VA_INVALID_SURFACE)
            vaDestroySurfaces(display_, &inputSurface_, 1);
        inputSurface_ = VA_INVALID_SURFACE;

        if (display_ && outputSurface_ != VA_INVALID_SURFACE)
            vaDestroySurfaces(display_, &outputSurface_, 1);
        outputSurface_ = VA_INVALID_SURFACE;

        if (display_ && config_ != VA_INVALID_ID)
            vaDestroyConfig(display_, config_);
        config_ = VA_INVALID_ID;

        if (display_) vaTerminate(display_);
        display_ = nullptr;

        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
        sw_ = sh_ = dw_ = dh_ = 0;
    }

    int fd_ = -1;
    VADisplay display_ = nullptr;
    VAConfigID config_ = VA_INVALID_ID;
    VAContextID context_ = VA_INVALID_ID;
    VASurfaceID inputSurface_ = VA_INVALID_SURFACE;
    VASurfaceID outputSurface_ = VA_INVALID_SURFACE;
    int sw_ = 0, sh_ = 0, dw_ = 0, dh_ = 0;
    bool ready_ = false;
    bool logged_ = false;
    bool fallbackLogged_ = false;
};
#endif

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
    std::vector<detail::HorizontalShuffleBlock> horizontalBlocks;

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
        // Build SSSE3 shuffle masks once per input/output geometry.
        // The scalar path remains active for unsupported spans/CPUs.
        horizontalBlocks.clear();
        horizontalBlocks.reserve(static_cast<std::size_t>(dw / 16));
        for (int x = 0; x + 16 <= dw; x += 16)
            horizontalBlocks.push_back(detail::prepareHorizontalShuffleBlock(
                x0.data(), x1.data(), wx.data(), sw, x));
    }
};

// A two-row separable implementation of the original fixed-point bilinear
// scaler. Horizontal contributions are kept at full 8-bit fractional
// precision (0..255*256); vertical interpolation then uses the *same*
// 32768 rounding and >>16 as the old four-tap expression. No color/quality
// approximation or changes to I420 geometry are introduced.
void scalePlane(const std::uint8_t* src, int sw, int sh, int sstride,
                std::uint8_t* dst, int dw, int dh, int dstride,
                bool chroma) {
    if (sw == dw && sh == dh) {
        for (int y = 0; y < sh; ++y)
            std::memcpy(dst + y * dstride, src + y * sstride,
                        static_cast<std::size_t>(sw));
        return;
    }

    // Luma and chroma use different coordinate maps. The old single
    // thread-local map alternated Y -> U -> V -> Y, rebuilding hundreds
    // of coordinate divisions on EVERY frame. Keep one map per geometry.
    thread_local BilinearMap lumaMap;
    thread_local BilinearMap chromaMap;
    auto& map = chroma ? chromaMap : lumaMap;
    map.prepare(sw, sh, dw, dh);

    // Temporary rows are retained per worker, but their content is never
    // reused across separate frames or even different U/V planes.
    thread_local std::array<std::vector<std::uint16_t>, 2> lumaRows;
    thread_local std::array<std::vector<std::uint16_t>, 2> chromaRows;
    auto& horizontal = chroma ? chromaRows : lumaRows;
    for (auto& row : horizontal) row.resize(static_cast<std::size_t>(dw));
    int cachedSrcRows[2] = {-1, -1};

    // In 576->1080 upscaling, neighboring output rows repeatedly address
    // the same source rows. Calculate each horizontal source row once, then
    // reuse it across as many vertical output rows as need it.
    const auto getHorizontal = [&](int sourceY, int preserveY)
        -> const std::uint16_t* {
        if (cachedSrcRows[0] == sourceY) return horizontal[0].data();
        if (cachedSrcRows[1] == sourceY) return horizontal[1].data();

        // Never evict the other source row needed by the current output row.
        const int slot = cachedSrcRows[0] == preserveY ? 1 : 0;
        const auto* inputRow = src + static_cast<std::size_t>(sourceY) *
                                    static_cast<std::size_t>(sstride);
        auto* out = horizontal[slot].data();
        detail::blendHorizontalRow(inputRow, map.x0.data(),
                                   map.x1.data(), map.wx.data(),
                                   map.horizontalBlocks, out, dw);
        cachedSrcRows[slot] = sourceY;
        return out;
    };

    for (int y = 0; y < dh; ++y) {
        const auto iy = static_cast<std::size_t>(y);
        const int srcY0 = map.y0[iy];
        const int srcY1 = map.y1[iy];
        const unsigned fy = map.wy[iy];
        const auto* top = getHorizontal(srcY0, srcY1);
        const auto* bottom = getHorizontal(srcY1, srcY0);
        auto* dstRow = dst + static_cast<std::size_t>(y) *
                                 static_cast<std::size_t>(dstride);
        detail::blendVerticalRow(top, bottom, dstRow, dw, fy);
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
#if defined(DVBSTREAMER5_HAVE_VAAPI)
    if (enableVaapiSoftwareScaling() &&
        (input.width != width || input.height != height)) {
        thread_local VaapiVppScaler vppScaler;
        if (vppScaler.scale(input, width, height, output))
            return true;
    }
#endif

    static const bool scalerPolicyLogged = [] {
        std::cerr << "NATIVE VIDEO SCALE software-frame backend=cpu"
                  << " vaapi_vpp_opt_in="
                  << (enableVaapiSoftwareScaling() ? 1 : 0)
                  << " ssse3_horizontal="
                  << (detail::horizontalSimdAvailable() ? 1 : 0)
                  << " avx2_vertical="
                  << (detail::verticalSimdAvailable() ? 1 : 0)
                  << " env=DVBSTREAMER5_ENABLE_VAAPI_SOFTWARE_SCALE"
                  << std::endl;
        return true;
    }();
    (void)scalerPolicyLogged;

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
    scalePlane(sy, input.width, input.height, input.width, dy, width, height, width, false);
    scalePlane(su, input.width / 2, input.height / 2, input.width / 2, du, width / 2, height / 2, width / 2, true);
    scalePlane(sv, input.width / 2, input.height / 2, input.width / 2, dv, width / 2, height / 2, width / 2, true);
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


bool deinterlaceBobFieldI420(const RawVideoFrame& input, bool topField,
                            RawVideoFrame& output) {
    if (!input.interlaced || input.width <= 0 || input.height < 4 ||
        (input.width & 1) || (input.height & 1)) return false;
    const std::size_t expected =
        static_cast<std::size_t>(input.width) * input.height * 3U / 2U;
    if (input.i420.size() < expected) return false;
    output = input;
    output.interlaced = false;
    std::size_t offset = 0;
    for (int plane = 0; plane < 3; ++plane) {
        const int w = plane == 0 ? input.width : input.width / 2;
        const int h = plane == 0 ? input.height : input.height / 2;
        const auto* src = input.i420.data() + offset;
        auto* dst = output.i420.data() + offset;
        const int parity = topField ? 0 : 1;
        for (int y = 0; y < h; ++y) {
            if ((y & 1) == parity) continue;
            int prev = y - 1;
            int next = y + 1;
            if (prev < 0) prev = next;
            if (next >= h) next = prev;
            for (int x = 0; x < w; ++x) {
                dst[static_cast<std::size_t>(y) * w + x] =
                    static_cast<std::uint8_t>(
                        (static_cast<unsigned>(src[static_cast<std::size_t>(prev) * w + x]) +
                         static_cast<unsigned>(src[static_cast<std::size_t>(next) * w + x]) + 1U) / 2U);
            }
        }
        offset += static_cast<std::size_t>(w) * h;
    }
    return true;
}

bool resamplePcm16(const PcmAudioFrame& input, int sampleRate, int channels,
                   PcmAudioFrame& output, std::string& error) {
    error.clear();
    if (input.sampleRate <= 0 || input.channels <= 0 || input.channels > 8 ||
        sampleRate <= 0 || channels <= 0 || channels > 8 ||
        input.samples.size() % static_cast<std::size_t>(input.channels) != 0) {
        error = "invalid native PCM resample parameters"; return false;
    }
    // Already 48 kHz/stereo for the common broadcast AAC path:
    // bypass floating-point interpolation and per-sample arithmetic.
    // Still copy the data because the caller supplies a const source frame.
    if (input.sampleRate == sampleRate && input.channels == channels) {
        output = input;
        return true;
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
