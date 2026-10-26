#include "media/NativeAc3Decoder.h"

#include <AC3Decoder.h>

#include <cstdint>
#include <iostream>
#include <memory>
#include <utility>
#include <vector>

namespace dvbstreamer5::media::codec {
namespace {

class NativeAc3AudioDecoder final : public AudioDecoder {
public:
    NativeAc3AudioDecoder() {
        decoder_.setPCMCallback(&NativeAc3AudioDecoder::pcmCallback, this);
        decoder_.setOutputChannels(2);
        ready_ = decoder_.begin();
    }

    ~NativeAc3AudioDecoder() override {
        decoder_.end();
    }

    bool valid() const noexcept {
        return ready_;
    }

    bool decode(const std::uint8_t* data, std::size_t size,
                std::uint64_t pts90k, bool hasPts,
                std::vector<PcmAudioFrame>& output,
                std::string& error) override {
        error.clear();
        if (!ready_) {
            error = "native AC-3 decoder is not initialized";
            return false;
        }
        if (!data || size == 0) return true;

        output_ = &output;
        if (hasPts) {
            nextPts90k_ = pts90k & kPtsMask;
            clockValid_ = true;
        }

        const std::size_t consumed = decoder_.write(data, size);
        output_ = nullptr;

        if (consumed != size) {
            error = "native AC-3 decoder did not consume the complete input";
            return false;
        }
        return true;
    }

    void reset() override {
        decoder_.end();
        decoder_.setPCMCallback(&NativeAc3AudioDecoder::pcmCallback, this);
        decoder_.setOutputChannels(2);
        ready_ = decoder_.begin();
        output_ = nullptr;
        clockValid_ = false;
        nextPts90k_ = 0;
    }

private:
    static constexpr std::uint64_t kPtsMask = (1ULL << 33U) - 1ULL;

    static void pcmCallback(const std::int16_t* pcm, int frameCount,
                            int channels, int sampleRate, void* opaque) {
        auto* self = static_cast<NativeAc3AudioDecoder*>(opaque);
        if (self) self->onPcm(pcm, frameCount, channels, sampleRate);
    }

    void onPcm(const std::int16_t* pcm, int frameCount,
               int channels, int sampleRate) {
        if (!output_ || !pcm || frameCount <= 0 ||
            channels <= 0 || sampleRate <= 0) {
            return;
        }

        PcmAudioFrame frame;
        frame.sampleRate = sampleRate;
        frame.channels = channels;
        frame.samples.assign(
            pcm, pcm + static_cast<std::size_t>(frameCount) *
                       static_cast<std::size_t>(channels));
        frame.hasPts = clockValid_;
        frame.pts90k = nextPts90k_;
        output_->push_back(std::move(frame));

        if (clockValid_) {
            nextPts90k_ =
                (nextPts90k_ +
                 static_cast<std::uint64_t>(frameCount) * 90000ULL /
                 static_cast<unsigned>(sampleRate)) &
                kPtsMask;
        }
    }

    AC3Decoder decoder_;
    std::vector<PcmAudioFrame>* output_ = nullptr;
    bool ready_ = false;
    bool clockValid_ = false;
    std::uint64_t nextPts90k_ = 0;
};

} // namespace

std::unique_ptr<AudioDecoder> createNativeAc3Decoder(std::string& error) {
    error.clear();
    auto decoder = std::make_unique<NativeAc3AudioDecoder>();
    if (!decoder->valid()) {
        error = "native AC-3 decoder initialization failed";
        return {};
    }
    std::cerr << "NATIVE AC3 DECODER selected=codec-ac3"
              << " output=stereo-downmix"
              << std::endl;
    return decoder;
}

} // namespace dvbstreamer5::media::codec
