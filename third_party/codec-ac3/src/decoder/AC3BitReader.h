#pragma once
#include <stdint.h>

namespace ac3 {

/// Simple MSB-first bit reader over a byte buffer, as used by the AC-3
/// bitstream syntax (all fields are packed MSB first, byte 0 first).
class AC3BitReader {
 public:
  void init(const uint8_t* data, int size_bytes) {
    data_ = data;
    size_bits_ = size_bytes * 8;
    pos_bits_ = 0;
  }

  /// Read 'n' bits (0..32) as an unsigned value.
  uint32_t getBits(int n) {
    uint32_t result = 0;
    while (n > 0) {
      if (pos_bits_ >= size_bits_) {
        pos_bits_ += n;
        return result << n;  // out of data: return zero-padded value
      }
      int byte_index = pos_bits_ >> 3;
      int bit_in_byte = pos_bits_ & 7;
      int bits_avail_in_byte = 8 - bit_in_byte;
      int take = (n < bits_avail_in_byte) ? n : bits_avail_in_byte;
      int shift = bits_avail_in_byte - take;
      uint32_t mask = (take == 8) ? 0xFF : ((1u << take) - 1u);
      uint32_t chunk = (data_[byte_index] >> shift) & mask;
      result = (result << take) | chunk;
      pos_bits_ += take;
      n -= take;
    }
    return result;
  }

  /// Read 'n' bits (1..32) as a two's-complement signed value.
  int32_t getBitsSigned(int n) {
    uint32_t v = getBits(n);
    if (n <= 0 || n >= 32) return (int32_t)v;
    uint32_t signBit = 1u << (n - 1);
    if (v & signBit) v |= ~((signBit << 1) - 1u);
    return (int32_t)v;
  }

  void skipBits(int n) { pos_bits_ += n; }

  int bitPosition() const { return pos_bits_; }
  void setBitPosition(int pos) { pos_bits_ = pos; }

  int bitsLeft() const { return size_bits_ - pos_bits_; }
  bool overflowed() const { return pos_bits_ > size_bits_; }

  /// Byte-align, then return a pointer to the current byte and remaining
  /// byte count (used to locate the start of the next frame etc.).
  int bytePosition() const { return (pos_bits_ + 7) >> 3; }

 private:
  const uint8_t* data_ = nullptr;
  int size_bits_ = 0;
  int pos_bits_ = 0;
};

}  // namespace ac3
