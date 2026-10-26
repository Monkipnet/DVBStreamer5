#pragma once
/**
 * Arduino-friendly, header-only AC-3 (Dolby Digital / ATSC A/52) decoder
 * facade. Feed it raw AC-3 elementary-stream bytes of any chunk size via
 * write(); it locates sync frames, decodes them, and delivers interleaved
 * PCM through a callback.
 *
 * The output sample type is a template parameter, resolved at compile
 * time (no runtime branching/conversion-table overhead):
 *
 *   AC3DecoderT<int16_t>   AC3Decoder            (the default, see below)
 *   AC3DecoderT<int32_t>
 *   AC3DecoderT<ac3::int24_t>   (packed 3-byte PCM24; see AC3Config.h)
 *   AC3DecoderT<float>     raw decoder output, no int conversion/clamping
 *
 * `AC3Decoder` is a plain type alias for AC3DecoderT<int16_t>, so existing
 * code that just writes `AC3Decoder decoder;` keeps working unchanged.
 *
 * This class has no dependency on any audio framework - it can be used
 * standalone, or easily wrapped by an AudioDecoder-style adapter (e.g. in
 * pschatzmann/arduino-audio-tools) since it already exposes the usual
 * begin()/write()/end()/operator bool() shape.
 *
 * Output channel order matches the AC-3 bitstream's own channel order:
 * full-bandwidth channels first, in the subset/order [L, C, R, SL, SR]
 * implied by the coded acmod (see channels()), followed by LFE last if
 * present. This does NOT match common PCM/WAV 5.1 channel order
 * (L,R,C,LFE,SL,SR) - reorder if you need that.
 */

#include <new>
#include <stdint.h>
#include <string.h>
#include "decoder/AC3Decoder.h"
#include "decoder/AC3Downmix.h"

namespace ac3 {

/// Converts one decoded float sample to the requested output type T,
/// clamping to the representable range first (except for T = float,
/// which is passed through unclamped/unscaled - the raw decoder output).
/// Specialize this for any additional output type you need.
template <typename T>
struct AC3SampleConvert;

template <>
struct AC3SampleConvert<float> {
  static float from(sample_t v) { return v; }
};

template <>
struct AC3SampleConvert<int16_t> {
  static int16_t from(sample_t v) {
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    return (int16_t)(v * 32767.0f);
  }
};

template <>
struct AC3SampleConvert<int24_t> {
  static int24_t from(sample_t v) {
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    return int24_t((int32_t)(v * 8388607.0f));  // 2^23 - 1
  }
};

template <>
struct AC3SampleConvert<int32_t> {
  static int32_t from(sample_t v) {
    // 2147483647.0f (2^31 - 1) isn't exactly representable as a float -
    // it rounds up to 2^31, one past INT32_MAX, which would make
    // (int32_t)(v * 2147483647.0f) undefined behavior right at the
    // clamp boundary. Special-case |v| >= 1 instead of relying on that
    // rounded scale factor.
    if (v >= 1.0f) return 2147483647;
    if (v <= -1.0f) return -2147483647;
    return (int32_t)(v * 2147483648.0f);  // 2^31, exact in float
  }
};

}  // namespace ac3

template <typename T = int16_t>
class AC3DecoderT {
 public:
  /// Receives one decoded audio block: `frameCount` interleaved PCM
  /// frames (always 256 for AC-3) of type T, `channels` samples per
  /// frame, at `sampleRate`.
  using PCMCallback = void (*)(const T* pcm, int frameCount, int channels,
                                int sampleRate, void* ref);

  /// Per-frame decode scratch space (~11KB for the default T=int16_t;
  /// scales with sizeof(T)): per-channel PCM and the interleaved output
  /// buffer (downmixing, when requested, happens in place within
  /// 'storage' - see ac3::AC3Decoder::setOutputChannels()). This keeps
  /// that memory off the stack - decodeFrame() never needs it as local
  /// stack arrays. Unless you call setScratchBuffer() with your own
  /// buffer, AC3DecoderT heap-allocates one of these in begin() and
  /// frees it in end(), so an idle/not-yet-begun decoder doesn't carry
  /// it permanently. Call setScratchBuffer() if you'd rather supply your
  /// own (e.g. to avoid heap use entirely, to place it in a specific
  /// memory region, or to share one buffer between instances that are
  /// never decoding concurrently).
  struct Scratch {
    ac3::sample_t storage[ac3::kAc3MaxChannels][ac3::kAc3BlockSamples];
    T interleaved[ac3::kAc3BlockSamples * ac3::kAc3MaxChannels];
  };

  /// Bytes a Scratch buffer occupies (sizeof(Scratch)), for callers who
  /// want to allocate/place one themselves.
  static constexpr size_t scratchBufferSize() { return sizeof(Scratch); }

  /// Supplies an external scratch buffer for write()/decodeFrame() to
  /// use, instead of the one that would otherwise be heap-allocated in
  /// begin(). 'scratch' must stay valid for as long as it's in use (e.g.
  /// a static/global variable); it is not owned or freed by this class.
  /// Pass nullptr to go back to the default heap-allocated-in-begin()
  /// behavior. Safe to call whether or not begin() has run yet.
  void setScratchBuffer(Scratch* scratch) {
    freeOwnedScratch();
    scratch_ = scratch;
  }

  ~AC3DecoderT() { freeOwnedScratch(); }

  /// Returns false if a scratch buffer needed to be heap-allocated and
  /// that allocation failed (e.g. out of memory); the decoder is not
  /// usable in that case.
  bool begin() {
    decoder_.reset();
    bufLen_ = 0;
    if (!scratch_) {
      scratch_ = new (std::nothrow) Scratch();
      if (!scratch_) {
        active_ = false;
        return false;
      }
      ownsScratch_ = true;
    }
    active_ = true;
    return true;
  }

  /// Stops the decoder and frees the scratch buffer if this instance
  /// allocated it itself in begin() (a buffer supplied via
  /// setScratchBuffer() is left untouched - it's the caller's).
  void end() {
    active_ = false;
    freeOwnedScratch();
  }

  operator bool() const { return active_; }

  /// Registers the callback that receives decoded PCM. 'ref' is passed
  /// through unchanged for the caller's own bookkeeping.
  void setPCMCallback(PCMCallback cb, void* ref = nullptr) {
    callback_ = cb;
    callbackRef_ = ref;
  }

  /// Requests that output be downmixed to 'n' channels (1 = mono, 2 =
  /// stereo) whenever the source has more channels than that. Pass 0
  /// (the default) to always output the full discrete channel set
  /// instead. LFE is not included in a downmix (it is not part of the
  /// standard downmix equations); it is simply dropped when downmixing.
  /// Requesting more channels than the source has is a no-op (you still
  /// get the source's discrete channel count).
  ///
  /// The downmix itself is done by the underlying ac3::AC3Decoder,
  /// before the IMDCT where possible (mixing spectra is far cheaper
  /// than running a full IMDCT per discrete channel and mixing PCM
  /// afterwards) - see ac3::AC3Decoder::setOutputChannels() for why.
  void setOutputChannels(int n) {
    outputChannels_ = n;
    decoder_.setOutputChannels(n);
  }

  /// The channel count that write()'s callback will actually deliver,
  /// given the current stream and setOutputChannels() request.
  int channels() const { return effectiveChannels(); }
  int sampleRate() const { return decoder_.sampleRate(); }

  /// Feeds raw AC-3 elementary-stream bytes (any chunk size / alignment
  /// - sync frames need not start at the beginning of a call). Decoded
  /// PCM for every complete frame found is delivered synchronously via
  /// the registered callback before this call returns. Always returns
  /// 'len' (all bytes are buffered/consumed).
  size_t write(const uint8_t* data, size_t len) {
    if (!active_) return 0;
    size_t offset = 0;
    while (offset < len) {
      size_t room = kBufCap - bufLen_;
      if (room == 0) {
        // Should not normally happen (a valid frame is <= kAc3MaxFrameBytes
        // and we always resync promptly); guard against runaway growth.
        bufLen_ = 0;
        room = kBufCap;
      }
      size_t take = len - offset;
      if (take > room) take = room;
      memcpy(buf_ + bufLen_, data + offset, take);
      bufLen_ += take;
      offset += take;
      processBuffer();
    }
    return len;
  }

 private:
  static const size_t kBufCap = ac3::kAc3MaxFrameBytes * 2;

  void processBuffer() {
    for (;;) {
      // Find the sync word within the buffered bytes.
      size_t syncPos = 0;
      bool found = false;
      if (bufLen_ >= 2) {
        for (; syncPos + 1 < bufLen_; syncPos++) {
          if (buf_[syncPos] == 0x0B && buf_[syncPos + 1] == 0x77) {
            found = true;
            break;
          }
        }
      }
      if (!found) {
        // Keep the last byte (could be the first half of a split sync
        // word) and discard the rest.
        if (bufLen_ > 0) {
          buf_[0] = buf_[bufLen_ - 1];
          bufLen_ = 1;
        }
        return;
      }
      if (syncPos > 0) {
        memmove(buf_, buf_ + syncPos, bufLen_ - syncPos);
        bufLen_ -= syncPos;
      }

      int frameSize = decoder_.parseHeader(buf_, (int)bufLen_);
      if (frameSize == 0) {
        // Either not enough data yet, or not a real sync frame here.
        if (bufLen_ < (size_t)ac3::kAc3MaxFrameBytes) return;  // wait for more
        // Buffer is full and still no valid frame at this position: skip
        // the false sync word and keep resyncing.
        memmove(buf_, buf_ + 2, bufLen_ - 2);
        bufLen_ -= 2;
        continue;
      }
      if (bufLen_ < (size_t)frameSize) return;  // wait for the rest of the frame

      decodeFrame();

      memmove(buf_, buf_ + frameSize, bufLen_ - frameSize);
      bufLen_ -= frameSize;
    }
  }

  /// Channels write() will deliver for the currently-parsed header, given
  /// the current setOutputChannels() request (0 = always passthrough).
  int effectiveChannels() const {
    int total = decoder_.channelCount();
    if (outputChannels_ <= 0 || outputChannels_ >= total) return total;
    if (outputChannels_ == 1 || outputChannels_ == 2) return outputChannels_;
    return total;  // unsupported downmix target: passthrough
  }

  void decodeFrame() {
    if (!scratch_) return;  // e.g. setScratchBuffer(nullptr) called mid-stream
    int srcCh = decoder_.channelCount();
    if (srcCh <= 0 || srcCh > ac3::kAc3MaxChannels) return;

    // All working memory below comes from *scratch_ (heap-allocated in
    // begin(), or a caller-supplied buffer) rather than the stack.
    ac3::sample_t* chanBufs[ac3::kAc3MaxChannels];
    for (int c = 0; c < srcCh; c++) chanBufs[c] = scratch_->storage[c];

    // decoder_.decodeBlock() already applies the setOutputChannels()
    // downmix itself (see ac3::AC3Decoder::decodeBlock()'s doc comment),
    // filling exactly effectiveChannels() of chanBufs - nothing left to
    // mix here.
    int outCh = effectiveChannels();
    T* interleaved = scratch_->interleaved;
    int sampleRate = decoder_.sampleRate();

    for (int b = 0; b < ac3::kAc3BlocksPerFrame; b++) {
      if (!decoder_.decodeBlock(chanBufs)) break;

      for (int s = 0; s < ac3::kAc3BlockSamples; s++)
        for (int c = 0; c < outCh; c++)
          interleaved[s * outCh + c] =
              ac3::AC3SampleConvert<T>::from(chanBufs[c][s]);
      if (callback_)
        callback_(interleaved, ac3::kAc3BlockSamples, outCh, sampleRate,
                   callbackRef_);
    }
  }

  /// Frees scratch_ only if this instance allocated it (in begin()); a
  /// caller-supplied buffer from setScratchBuffer() is left as-is (not
  /// freed, and still remembered for the next begin()).
  void freeOwnedScratch() {
    if (ownsScratch_) {
      delete scratch_;
      scratch_ = nullptr;
      ownsScratch_ = false;
    }
  }

  ac3::AC3Decoder decoder_;
  bool active_ = false;
  int outputChannels_ = 0;  // 0 = passthrough discrete channels
  PCMCallback callback_ = nullptr;
  void* callbackRef_ = nullptr;
  uint8_t buf_[kBufCap];
  size_t bufLen_ = 0;

  Scratch* scratch_ = nullptr;
  bool ownsScratch_ = false;
};

/// Default 16-bit-PCM decoder - matches the pre-template AC3Decoder API.
using AC3Decoder = AC3DecoderT<int16_t>;
