#include "media/NativeIttiamAvc.h"

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
    // The pinned upstream Ittiam libavc x86 build is compiled globally with
    // -msse4.2 -mavx2 -mfma.  Entering that static library on an older CPU can
    // therefore raise SIGILL before libavc gets a chance to select an optimized
    // implementation.  Guard the factory before the first Ittiam call and let
    // the existing OpenH264 fallback handle CPUs without the required ISA.
    __builtin_cpu_init();
    std::string missing;
    const auto addMissing = [&missing](const char* feature) {
        if (!missing.empty()) missing += ',';
        missing += feature;
    };
    if (!__builtin_cpu_supports("sse4.2")) addMissing("sse4.2");
    if (!__builtin_cpu_supports("avx2")) addMissing("avx2");
    if (!__builtin_cpu_supports("fma")) addMissing("fma");
    if (!missing.empty()) {
        reason = "CPU missing Ittiam-required SIMD: " + missing;
        return false;
    }
    return true;
#else
    reason = "Ittiam CPU feature detection unavailable on x86";
    return false;
#endif
#else
    // The upstream ARM builds use their architecture-specific implementation;
    // the x86 SSE4.2/AVX2/FMA requirement does not apply there.
    return true;
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

        constexpr std::uint64_t kPtsMask = (1ULL << 33U) - 1ULL;
        constexpr std::uint64_t kPalFrameInterval90k = 90000ULL / 25ULL;

        for (auto& frame : decoded) {
            if (frame.interlaced && frame.hasPts) {
                const std::uint64_t incomingPts = frame.pts90k & kPtsMask;
                if (!clockValid_) {
                    clockValid_ = true;
                    nextPts90k_ = incomingPts;
                    displayFrames_ = 0;
                    std::cerr << "NATIVE INTERLACED DISPLAY CLOCK start pts="
                              << incomingPts
                              << " interval=" << kPalFrameInterval90k
                              << " fps=25" << std::endl;
                }

                frame.pts90k = nextPts90k_;
                frame.dts90k = frame.pts90k;
                frame.hasDts = true;
                nextPts90k_ =
                    (nextPts90k_ + kPalFrameInterval90k) & kPtsMask;

                ++displayFrames_;
                if (displayFrames_ == 1U || (displayFrames_ % 250U) == 0U) {
                    std::cerr << "NATIVE INTERLACED DISPLAY CLOCK frames="
                              << displayFrames_
                              << " pts=" << frame.pts90k
                              << " next=" << nextPts90k_
                              << std::endl;
                }
            } else if (!frame.interlaced) {
                // A later switch back to field-coded AVC must re-anchor to the
                // new source timeline rather than continuing an old clock.
                clockValid_ = false;
                displayFrames_ = 0;
            }
            output.push_back(std::move(frame));
        }
        return true;
    }

    void reset() override {
        if (inner_) inner_->reset();
        clockValid_ = false;
        nextPts90k_ = 0;
        displayFrames_ = 0;
    }

private:
    std::unique_ptr<VideoDecoder> inner_;
    bool clockValid_ = false;
    std::uint64_t nextPts90k_ = 0;
    std::uint64_t displayFrames_ = 0;
};

} // namespace

std::unique_ptr<VideoDecoder> createIttiamAvcDecoder(std::string& error) {
    if (!ittiamAvcCpuSupported(error)) {
        std::cerr << "NATIVE AVC ITTIAM skipped reason=" << error << '\n';
        return {};
    }

    auto inner = createIttiamAvcDecoderBase(error);
    if (!inner) return {};
    error.clear();
    return std::make_unique<IttiamInterlacedDisplayClockDecoder>(std::move(inner));
}

} // namespace dvbstreamer5::media::codec
