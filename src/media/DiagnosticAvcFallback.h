#pragma once

// Isolated diagnostic A/B wrapper. Try OpenH264 for the selected stream,
// automatically restore the known-working Ittiam decoder if OpenH264 fails
// or stops producing frames. No changes to default decoder selection.
#include "media/NativeCodecRuntime.h"

#include <cstddef>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

namespace dvbstreamer5::media::codec {

class DiagnosticAvcFallbackDecoder final : public VideoDecoder {
public:
    using Factory = std::function<std::unique_ptr<VideoDecoder>(std::string&)>;
    static constexpr std::size_t kMaxNoOutputSamples = 150;

    DiagnosticAvcFallbackDecoder(std::unique_ptr<VideoDecoder> diagnostic,
                                 Factory fallback, std::string streamId)
        : active_(std::move(diagnostic)),
          fallbackFactory_(std::move(fallback)),
          streamId_(std::move(streamId)) {}

    bool decode(const std::uint8_t* data, std::size_t size,
                std::uint64_t pts90k, bool hasPts,
                std::vector<RawVideoFrame>& output,
                std::string& error) override {
        error.clear();
        if (!active_) {
            error = "diagnostic AVC decoder is not initialized";
            return false;
        }
        if (fallbackActive_) return active_->decode(
            data, size, pts90k, hasPts, output, error);

        const std::size_t before = output.size();
        std::string diagnosticError;
        const bool ok = active_->decode(
            data, size, pts90k, hasPts, output, diagnosticError);
        if (ok && output.size() > before) {
            noOutputSamples_ = 0;
            return true;
        }
        // Do not count empty PES/sample calls towards decoder starvation.
        if (ok && (!data || size == 0)) return true;
        if (ok && ++noOutputSamples_ < kMaxNoOutputSamples) return true;

        const std::string reason = ok
            ? "no_decoded_frames_after_" + std::to_string(noOutputSamples_) + "_samples"
            : "decode_error:" + diagnosticError;
        // A failing decoder could have partially appended frames before
        // reporting failure. Drop those frames before switching backends.
        output.resize(before);
        if (!useFallback(reason, error)) return false;
        return active_->decode(data, size, pts90k, hasPts, output, error);
    }

    void reset() override {
        noOutputSamples_ = 0;
        if (active_) active_->reset();
    }

private:
    bool useFallback(const std::string& reason, std::string& error) {
        std::string fallbackError;
        auto fallback = fallbackFactory_
            ? fallbackFactory_(fallbackError) : nullptr;
        if (!fallback) {
            error = "OpenH264 diagnostic fallback to Ittiam failed: " +
                fallbackError + " (initial cause=" + reason + ")";
            std::cerr << "NATIVE AVC DECODER diagnostic fallback_failed=ittiam-libavc"
                      << " stream_id=" << streamId_
                      << " cause=" << reason
                      << " error=" << fallbackError << std::endl;
            return false;
        }
        active_ = std::move(fallback);
        fallbackActive_ = true;
        noOutputSamples_ = 0;
        std::cerr << "NATIVE AVC DECODER diagnostic fallback=ittiam-libavc"
                  << " stream_id=" << streamId_
                  << " cause=" << reason
                  << " output_encoder=unchanged" << std::endl;
        return true;
    }

    std::unique_ptr<VideoDecoder> active_;
    Factory fallbackFactory_;
    std::string streamId_;
    std::size_t noOutputSamples_ = 0;
    bool fallbackActive_ = false;
};

} // namespace dvbstreamer5::media::codec
