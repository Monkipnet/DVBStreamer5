#include "media/NativeIttiamAvc.h"

#include <cstdint>
#include <iostream>
#include <memory>
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
    auto inner = createIttiamAvcDecoderBase(error);
    if (!inner) return {};
    error.clear();
    return std::make_unique<IttiamInterlacedDisplayClockDecoder>(std::move(inner));
}

} // namespace dvbstreamer5::media::codec
