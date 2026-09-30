#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dvbstreamer5::media::hls {

// Apple HLS SAMPLE-AES transform for MPEG-TS H.264/H.265/AAC elementary samples.
// The same function is used for encryption and decryption. MPEG-TS PSI/PES is
// rebuilt by the native mux so transport continuity remains valid.
bool transformSampleAesMpegTs(const std::uint8_t* data, std::size_t size,
                              const std::array<std::uint8_t, 16>& key,
                              const std::array<std::uint8_t, 16>& iv,
                              bool encrypt,
                              std::vector<std::uint8_t>& output,
                              std::string& error);

bool parseHexKey16(const std::string& value, std::array<std::uint8_t, 16>& key);

} // namespace dvbstreamer5::media::hls
