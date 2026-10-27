#include "media/Mp3Encoder.h"

#include <algorithm>
#include <array>
#include <limits>

#if defined(DVBSTREAMER5_HAVE_MP3LAME)
#include <lame/lame.h>
#endif

namespace dvbstreamer5::media {
namespace {
constexpr std::size_t kSamplesPerFrame = 1152;
constexpr std::size_t kOutputBufferBytes = 16384;

bool supportedBitrate(std::uint32_t bitrate) {
    switch (bitrate) {
        case 96000: case 128000: case 160000:
        case 192000: case 256000: case 320000: return true;
        default: return false;
    }
}
}

struct Mp3Encoder::Impl {
#if defined(DVBSTREAMER5_HAVE_MP3LAME)
    lame_t context = nullptr;
    ~Impl() { if (context) lame_close(context); }
#else
    ~Impl() = default;
#endif
    bool initialized = false;
    bool finished = false;
    std::size_t frameBytes = 0;
    std::vector<std::uint8_t> pending;
};

Mp3Encoder::Mp3Encoder() : impl_(std::make_unique<Impl>()) {}
Mp3Encoder::~Mp3Encoder() = default;

bool Mp3Encoder::available() noexcept {
#if defined(DVBSTREAMER5_HAVE_MP3LAME)
    return true;
#else
    return false;
#endif
}

bool Mp3Encoder::initialize(std::uint32_t sampleRate, std::uint32_t channels,
                            std::uint32_t bitrate, std::string& error) {
    error.clear();
    if (!impl_ || impl_->initialized) {
        error = "MP3 encoder already initialized";
        return false;
    }
    // The pipeline resamples all decoded audio to 48 kHz stereo.
    if (sampleRate != 48000 || channels != 2 || !supportedBitrate(bitrate)) {
        error = "MP3 supports 48 kHz stereo at 96/128/160/192/256/320 kbit/s";
        return false;
    }
#if defined(DVBSTREAMER5_HAVE_MP3LAME)
    impl_->context = lame_init();
    if (!impl_->context) {
        error = "LAME could not allocate encoder state";
        return false;
    }
    if (lame_set_in_samplerate(impl_->context, static_cast<int>(sampleRate)) < 0 ||
        lame_set_num_channels(impl_->context, static_cast<int>(channels)) < 0 ||
        lame_set_brate(impl_->context, static_cast<int>(bitrate / 1000)) < 0 ||
        lame_set_bWriteVbrTag(impl_->context, 0) < 0 ||
        lame_set_quality(impl_->context, 5) < 0 ||
        lame_init_params(impl_->context) < 0) {
        error = "LAME rejected the requested MP3 profile";
        return false;
    }
    impl_->frameBytes = static_cast<std::size_t>(144ULL * bitrate / sampleRate);
    impl_->initialized = true;
    return true;
#else
    error = "MP3 encoder not built: install libmp3lame-dev and rerun CMake";
    return false;
#endif
}

bool Mp3Encoder::encodeInterleaved(const std::int16_t* pcm,
                                   std::size_t samplesPerChannel,
                                   std::vector<std::uint8_t>& output,
                                   std::string& error) {
    error.clear();
    if (!impl_ || !impl_->initialized || impl_->finished) {
        error = "MP3 encoder is not active";
        return false;
    }
    if (samplesPerChannel == 0) return true;
    if (!pcm) {
        error = "MP3 PCM input is null";
        return false;
    }
    if (samplesPerChannel > std::numeric_limits<std::size_t>::max() / 2) {
        error = "MP3 PCM sample count overflow";
        return false;
    }
#if defined(DVBSTREAMER5_HAVE_MP3LAME)
    std::array<unsigned char, kOutputBufferBytes> encoded {};
    // LAME API takes a mutable short*; use a small local copy of each 1152
    // sample block to keep the decoder's source PCM immutable.
    std::array<short, kSamplesPerFrame * 2> block {};
    static_assert(sizeof(short) == sizeof(std::int16_t), "LAME expects 16-bit PCM");
    for (std::size_t offset = 0; offset < samplesPerChannel;) {
        const std::size_t n = std::min(kSamplesPerFrame, samplesPerChannel - offset);
        std::copy_n(pcm + offset * 2, n * 2, block.data());
        const int result = lame_encode_buffer_interleaved(
            impl_->context, block.data(), static_cast<int>(n),
            encoded.data(), static_cast<int>(encoded.size()));
        if (result < 0 || static_cast<std::size_t>(result) > encoded.size()) {
            error = "LAME MP3 PCM encoding failed (" + std::to_string(result) + ")";
            return false;
        }
        impl_->pending.insert(impl_->pending.end(), encoded.begin(), encoded.begin() + result);
        // LAME can split an MPEG audio frame across encode calls. Never send
        // a partial MP3 frame in a PES with a new audio PTS.
        const std::size_t complete =
            (impl_->pending.size() / impl_->frameBytes) * impl_->frameBytes;
        output.insert(output.end(), impl_->pending.begin(),
                      impl_->pending.begin() + static_cast<std::ptrdiff_t>(complete));
        impl_->pending.erase(impl_->pending.begin(),
                             impl_->pending.begin() + static_cast<std::ptrdiff_t>(complete));
        offset += n;
    }
    return true;
#else
    (void)pcm;
    (void)samplesPerChannel;
    (void)output;
    error = "MP3 encoder was not built";
    return false;
#endif
}

bool Mp3Encoder::finish(std::vector<std::uint8_t>& output, std::string& error) {
    error.clear();
    if (!impl_ || !impl_->initialized || impl_->finished) {
        error = "MP3 encoder is not active";
        return false;
    }
#if defined(DVBSTREAMER5_HAVE_MP3LAME)
    std::array<unsigned char, kOutputBufferBytes> encoded {};
    const int result = lame_encode_flush(
        impl_->context, encoded.data(), static_cast<int>(encoded.size()));
    if (result < 0 || static_cast<std::size_t>(result) > encoded.size()) {
        error = "LAME MP3 flush failed (" + std::to_string(result) + ")";
        return false;
    }
    impl_->pending.insert(impl_->pending.end(), encoded.begin(), encoded.begin() + result);
    if (impl_->pending.size() % impl_->frameBytes != 0) {
        error = "LAME returned an incomplete final MPEG audio frame";
        return false;
    }
    output.insert(output.end(), impl_->pending.begin(), impl_->pending.end());
    impl_->pending.clear();
    impl_->finished = true;
    return true;
#else
    (void)output;
    error = "MP3 encoder was not built";
    return false;
#endif
}

} // namespace dvbstreamer5::media

std::size_t dvbstreamer5::media::Mp3Encoder::frameSizeBytes() const noexcept {
    return impl_ ? impl_->frameBytes : 0;
}
