#include "media/NativeIttiamAvc.h"
#include "media/FfmpegPalPtsClock.h"

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// Build the validated Ittiam implementation under an internal factory name,
// then expose the normal factory through a thin display-clock wrapper.
// This keeps the decoder byte-feeding/recovery path unchanged while ensuring
// field-coded interlaced AVC produces a monotonic 25-fps display timeline.
#define createIttiamAvcDecoder createIttiamAvcDecoderBase
#include "media/NativeIttiamAvcBase.inc"
#undef createIttiamAvcDecoder

namespace dvbstreamer5::media::codec {
namespace {

bool ittiamAvcCpuSupported(std::string& reason) {
    reason.clear();
#if defined(__i386__) || defined(__x86_64__)
#if defined(__GNUC__) || defined(__clang__)
    // V10.8.146: the vendored libavc x86 build now has an SSE4.2-safe baseline.
    // Only its explicit AVX2 implementation files are compiled with AVX2/FMA,
    // and the library selector enters them only after runtime CPU detection.
    // Therefore the factory itself only needs to reject pre-SSE4.2 x86 CPUs.
    __builtin_cpu_init();
    if (!__builtin_cpu_supports("sse4.2")) {
        reason = "CPU missing Ittiam-required SIMD: sse4.2";
        return false;
    }
    return true;
#else
    reason = "Ittiam CPU feature detection unavailable on x86";
    return false;
#endif
#else
    // ARM builds retain the upstream architecture-specific implementation.
    return true;
#endif
}

const char* ittiamAvcRuntimeArch() {
#if defined(__i386__) || defined(__x86_64__)
#if defined(__GNUC__) || defined(__clang__)
    __builtin_cpu_init();
    if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma")) {
        return "avx2-fma";
    }
#endif
    return "sse4.2";
#else
    return "native";
#endif
}

class IttiamInterlacedDisplayClockDecoder final : public VideoDecoder {
public:
    explicit IttiamInterlacedDisplayClockDecoder(std::unique_ptr<VideoDecoder> inner)
        : inner_(std::move(inner)) {}

    bool decode(const std::uint8_t* data, std::size_t size,
                std::uint64_t pts90k, bool hasPts,
                std::vector<RawVideoFrame>& output,
                std::string& error) override {
        std::vector<RawVideoFrame> decoded;
        if (!inner_ || !inner_->decode(data, size, pts90k, hasPts, decoded, error))
            return false;

        // Do NOT synthesize a 25-fps timeline from the first decoded field
        // for every interlaced AVC stream. It may be 29.97/30 or contain an
        // incomplete field pair. The verified cadence clock only locks when
        // the source actually delivers approximately 25 full frames/s.
        // This avoids sending intentionally wrong PTS to the FPS gate/mux.
        for (auto& frame : decoded) {
            if (frame.interlaced && frame.hasPts) {
                const auto pal = palClock_.apply(frame.pts90k);
                if (pal.discontinuity) {
                    std::cerr << "NATIVE AVC ITTIAM PTS discontinuity"
                              << " source_pts=" << frame.pts90k
                              << " action=reanchor" << std::endl;
                }
                if (pal.corrected) {
                    frame.pts90k = pal.pts90k;
                    frame.dts90k = pal.pts90k;
                    frame.hasDts = true;
                    ++correctedFrames_;
                    if (pal.locked || (correctedFrames_ % 500U) == 0U)
                        std::cerr << "NATIVE AVC ITTIAM PAL cadence"
                                  << " locked=1 corrected=" << correctedFrames_
                                  << " source_delta90k=" << pal.sourceDelta90k
                                  << std::endl;
                }
            } else if (!frame.interlaced) {
                palClock_.reset();
                correctedFrames_ = 0;
            }
            output.push_back(std::move(frame));
        }
        return true;
    }

    void reset() override {
        if (inner_) inner_->reset();
        palClock_.reset();
        correctedFrames_ = 0;
    }

private:
    std::unique_ptr<VideoDecoder> inner_;
    FfmpegPalPtsClock palClock_;
    std::uint64_t correctedFrames_ = 0;
};

} // namespace

std::unique_ptr<VideoDecoder> createIttiamAvcDecoder(std::string& error) {
    if (!ittiamAvcCpuSupported(error)) {
        std::cerr << "NATIVE AVC ITTIAM skipped reason=" << error << '\n';
        return {};
    }

    std::cerr << "NATIVE AVC ITTIAM runtime-arch="
              << ittiamAvcRuntimeArch() << '\n';
    auto inner = createIttiamAvcDecoderBase(error);
    if (!inner) return {};
    error.clear();
    return std::make_unique<IttiamInterlacedDisplayClockDecoder>(std::move(inner));
}

} // namespace dvbstreamer5::media::codec
