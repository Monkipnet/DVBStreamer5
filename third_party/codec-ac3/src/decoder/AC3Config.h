#pragma once
/**
 * Core types and constants for the AC-3 (ATSC A/52) decoder.
 *
 * This is an original, header-only, clean-room implementation of the
 * ATSC A/52 audio decoding algorithm, written for embedded targets
 * (ESP32, RP2040, ...). The bitstream syntax and the numeric constant
 * tables (quantization tables, bit-allocation tables, exponent delta
 * tables, window function, ...) are mandated by the public ATSC A/52
 * standard and therefore are necessarily the same across independent
 * implementations (liba52, ffmpeg, ...); the code structure, naming and
 * organization here are original.
 */

#include <stdint.h>
#include <string.h>

namespace ac3 {

/// Internal sample representation used throughout the decoder pipeline.
using sample_t = float;

/// Packed signed 24-bit PCM sample (3 bytes, little-endian) - C++ has no
/// native 24-bit integer type, so this is what AC3Decoder<int24_t> (see
/// AC3Decoder.h) uses for 24-bit output, matching common PCM24/I2S byte
/// packing (e.g. WAV's 24-bit format, or many I2S DAC 24-bit modes).
struct int24_t {
  int24_t() = default;
  int24_t(int32_t v) {
    b[0] = (uint8_t)v;
    b[1] = (uint8_t)(v >> 8);
    b[2] = (uint8_t)(v >> 16);
  }
  operator int32_t() const {
    int32_t v = (int32_t)((uint32_t)b[0] | ((uint32_t)b[1] << 8) |
                           ((uint32_t)b[2] << 16));
    if (v & 0x00800000) v |= (int32_t)0xFF000000u;  // sign-extend 24 -> 32
    return v;
  }
  uint8_t b[3];
};

static const int kAc3MaxChannels = 6;    ///< L,C,R,SL,SR,LFE
static const int kAc3BlockSamples = 256; ///< samples per channel per audio block
static const int kAc3BlocksPerFrame = 6; ///< audio blocks per AC-3 frame
static const int kAc3MaxFrameSamples = kAc3BlockSamples * kAc3BlocksPerFrame; // 1536
static const int kAc3MaxFrameBytes = 3840; ///< largest legal AC-3 frame size

/// Audio coding mode (acmod) - number/layout of full-bandwidth channels.
enum AC3ChannelMode {
  kModeDualMono = 0, // 1+1  (Ch1, Ch2)
  kModeMono = 1,     // 1/0  (C)
  kModeStereo = 2,   // 2/0  (L, R)
  kMode3F = 3,       // 3/0  (L, C, R)
  kMode2F1R = 4,     // 2/1  (L, R, S)
  kMode3F1R = 5,     // 3/1  (L, C, R, S)
  kMode2F2R = 6,     // 2/2  (L, R, SL, SR)
  kMode3F2R = 7      // 3/2  (L, C, R, SL, SR)
};

/// Exponent coding strategy.
enum AC3ExpStrategy { kExpReuse = 0, kExpD15 = 1, kExpD25 = 2, kExpD45 = 3 };

/// Delta bit allocation "exists" flag values.
enum AC3DeltaBitMode {
  kDeltaBitReuse = 0,
  kDeltaBitNew = 1,
  kDeltaBitNone = 2,
  kDeltaBitReserved = 3
};

/// Number of full-bandwidth channels for each acmod (0..7).
static inline int ac3NumFbwChannels(int acmod) {
  static const uint8_t tbl[8] = {2, 1, 2, 3, 3, 4, 4, 5};
  return tbl[acmod & 7];
}

/// True if this acmod has a center channel.
static inline bool ac3HasCenter(int acmod) {
  return acmod == kMode3F || acmod == kMode3F1R || acmod == kMode3F2R;
}

/// True if this acmod has surround channel(s).
static inline bool ac3HasSurround(int acmod) {
  return acmod == kMode2F1R || acmod == kMode3F1R || acmod == kMode2F2R ||
         acmod == kMode3F2R;
}

/// True if this acmod has two independent surround channels (vs. one mono).
static inline bool ac3HasTwoSurround(int acmod) {
  return acmod == kMode2F2R || acmod == kMode3F2R;
}

}  // namespace ac3
