#include "media/Mp2Encoder.h"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

#include <twolame.h>

namespace dvbstreamer5::media {
namespace {

constexpr std::size_t kSamplesPerFrame = TWOLAME_SAMPLES_PER_FRAME;
constexpr std::size_t kOutputBufferSize = 8192;
constexpr std::array<std::uint32_t, 3> kSupportedSampleRates = {
    32000, 44100, 48000
};
constexpr std::array<std::uint32_t, 14> kSupportedBitrates = {
    32000, 48000, 56000, 64000, 80000, 96000, 112000,
    128000, 160000, 192000, 224000, 256000, 320000, 384000
};

template <std::size_t N>
bool contains(const std::array<std::uint32_t, N>& values, std::uint32_t value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

static_assert(sizeof(short int) == sizeof(std::int16_t),
              "TwoLAME requires 16-bit short PCM samples");

bool appendResult(int result,
                  const std::array<unsigned char, kOutputBufferSize>& buffer,
                  std::vector<std::uint8_t>& output,
                  std::string& error) {
    if (result < 0) {
        error = "TwoLAME failed to encode PCM";
        return false;
    }
    if (static_cast<std::size_t>(result) > buffer.size()) {
        error = "TwoLAME output exceeded the adapter buffer";
        return false;
    }
    output.insert(output.end(), buffer.begin(), buffer.begin() + result);
    return true;
}

} // namespace

struct Mp2Encoder::Impl {
    twolame_options* options = nullptr;
    std::uint32_t channels = 0;
    bool initialized = false;
    bool finished = false;

    ~Impl() {
        if (options) twolame_close(&options);
    }
};

Mp2Encoder::Mp2Encoder() : impl_(std::make_unique<Impl>()) {}
Mp2Encoder::~Mp2Encoder() = default;
Mp2Encoder::Mp2Encoder(Mp2Encoder&&) noexcept = default;
Mp2Encoder& Mp2Encoder::operator=(Mp2Encoder&&) noexcept = default;

bool Mp2Encoder::initialize(const Mp2EncoderConfig& config, std::string& error) {
    error.clear();
    if (!impl_) {
        error = "MP2 encoder has been moved from";
        return false;
    }
    if (impl_->initialized) {
        error = "MP2 encoder is already initialized";
        return false;
    }
    if (!contains(kSupportedSampleRates, config.sampleRate)) {
        error = "MP2 encoder supports sample rates 32000, 44100, and 48000 Hz";
        return false;
    }
    if (config.channels != 1 && config.channels != 2) {
        error = "MP2 encoder supports mono or stereo PCM";
        return false;
    }
    if (!contains(kSupportedBitrates, config.bitrate)) {
        error = "MP2 bitrate must be a supported MPEG-1 Layer II bitrate from 32 to 384 kbit/s";
        return false;
    }

    impl_->options = twolame_init();
    if (!impl_->options) {
        error = "TwoLAME could not allocate encoder state";
        return false;
    }

    const int setResult =
        twolame_set_num_channels(impl_->options, static_cast<int>(config.channels)) |
        twolame_set_in_samplerate(impl_->options, static_cast<int>(config.sampleRate)) |
        twolame_set_out_samplerate(impl_->options, static_cast<int>(config.sampleRate)) |
        twolame_set_bitrate(impl_->options, static_cast<int>(config.bitrate / 1000)) |
        twolame_set_mode(impl_->options,
            config.channels == 1 ? TWOLAME_MONO : TWOLAME_JOINT_STEREO) |
        twolame_set_verbosity(impl_->options, 0);
    if (setResult != 0 || twolame_init_params(impl_->options) != 0) {
        twolame_close(&impl_->options);
        error = "TwoLAME rejected the requested MPEG-1 Layer II profile";
        return false;
    }

    impl_->channels = config.channels;
    impl_->initialized = true;
    return true;
}

bool Mp2Encoder::encodeInterleaved(const std::int16_t* pcm,
                                   std::size_t samplesPerChannel,
                                   std::vector<std::uint8_t>& output,
                                   std::string& error) {
    error.clear();
    if (!impl_ || !impl_->initialized || impl_->finished) {
        error = "MP2 encoder is not active";
        return false;
    }
    if (samplesPerChannel == 0) return true;
    if (!pcm) {
        error = "PCM input is null";
        return false;
    }
    if (samplesPerChannel > std::numeric_limits<std::size_t>::max() / impl_->channels) {
        error = "PCM sample count overflows the interleaved input size";
        return false;
    }

    std::array<unsigned char, kOutputBufferSize> encoded {};
    std::size_t offset = 0;
    while (offset < samplesPerChannel) {
        const auto count = std::min(kSamplesPerFrame, samplesPerChannel - offset);
        const int result = twolame_encode_buffer_interleaved(
            impl_->options,
            reinterpret_cast<const short int*>(pcm + offset * impl_->channels),
            static_cast<int>(count),
            encoded.data(),
            static_cast<int>(encoded.size()));
        if (!appendResult(result, encoded, output, error)) return false;
        offset += count;
    }
    return true;
}

bool Mp2Encoder::finish(std::vector<std::uint8_t>& output, std::string& error) {
    error.clear();
    if (!impl_ || !impl_->initialized || impl_->finished) {
        error = "MP2 encoder is not active";
        return false;
    }

    std::array<unsigned char, kOutputBufferSize> encoded {};
    const int result = twolame_encode_flush(
        impl_->options, encoded.data(), static_cast<int>(encoded.size()));
    impl_->finished = true;
    return appendResult(result, encoded, output, error);
}

} // namespace dvbstreamer5::media
