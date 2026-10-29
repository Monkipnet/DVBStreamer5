// Build: g++ -std=c++17 -Isrc tests/test_diagnostic_avc_fallback.cpp -o /tmp/test_diagnostic_avc_fallback
#include "media/DiagnosticAvcFallback.h"
#include <cassert>
#include <memory>
#include <string>
#include <vector>
#include <iostream>

using namespace dvbstreamer5::media::codec;

class StubDecoder final : public VideoDecoder {
public:
    enum Mode { Silent, Output, Failure };
    explicit StubDecoder(Mode m) : mode(m) {}
    bool decode(const std::uint8_t*, std::size_t,
                std::uint64_t pts, bool hasPts,
                std::vector<RawVideoFrame>& output, std::string& error) override {
        if (mode == Failure) { error = "deliberate decode failure"; return false; }
        if (mode == Output) {
            RawVideoFrame frame;
            frame.width = 2; frame.height = 2;
            frame.i420.assign(6, 128);
            frame.pts90k = pts;
            frame.hasPts = hasPts;
            output.push_back(std::move(frame));
        }
        return true;
    }
    void reset() override {}
    Mode mode;
};

int main() {
    const std::uint8_t sample[] = {0,0,0,1,0x65,0};
    std::vector<RawVideoFrame> frames;
    std::string error;
    int fallbackCalls = 0;
    auto fallback = [&fallbackCalls](std::string&) -> std::unique_ptr<VideoDecoder> {
        ++fallbackCalls;
        return std::make_unique<StubDecoder>(StubDecoder::Output);
    };

    {
        DiagnosticAvcFallbackDecoder decoder(
            std::make_unique<StubDecoder>(StubDecoder::Silent), fallback, "stream-test");
        for (std::size_t i = 0;
             i + 1 < DiagnosticAvcFallbackDecoder::kMaxNoOutputSamples; ++i) {
            assert(decoder.decode(sample, sizeof(sample), i, true, frames, error));
            assert(frames.empty());
        }
        assert(fallbackCalls == 0);
        assert(decoder.decode(sample, sizeof(sample), 1000, true, frames, error));
        assert(fallbackCalls == 1 && frames.size() == 1);
        assert(frames.back().pts90k == 1000);
        frames.clear();
        assert(decoder.decode(sample, sizeof(sample), 2000, true, frames, error));
        assert(fallbackCalls == 1 && frames.size() == 1);
        frames.clear();
    }
    {
        DiagnosticAvcFallbackDecoder decoder(
            std::make_unique<StubDecoder>(StubDecoder::Failure), fallback, "stream-test");
        assert(decoder.decode(sample, sizeof(sample), 3000, true, frames, error));
        assert(error.empty());
        assert(fallbackCalls == 2 && frames.size() == 1);
        frames.clear();
    }
    {
        DiagnosticAvcFallbackDecoder decoder(
            std::make_unique<StubDecoder>(StubDecoder::Output), fallback, "stream-test");
        for (int i = 0; i < 250; ++i) {
            frames.clear();
            assert(decoder.decode(sample, sizeof(sample), i, true, frames, error));
            assert(frames.size() == 1);
        }
        assert(fallbackCalls == 2);
    }
    {
        // A diagnostic decoder that cannot initialize a fallback must
        // report a real error, not pretend the stream is healthy.
        DiagnosticAvcFallbackDecoder decoder(
            std::make_unique<StubDecoder>(StubDecoder::Failure),
            [](std::string& err) -> std::unique_ptr<VideoDecoder> {
                err = "test fallback unavailable"; return {};
            }, "stream-test");
        frames.clear();
        assert(!decoder.decode(sample, sizeof(sample), 0, true, frames, error));
        assert(error.find("test fallback unavailable") != std::string::npos);
    }
    std::cout << "Diagnostic OpenH264 -> Ittiam fallback tests passed\n";
}
