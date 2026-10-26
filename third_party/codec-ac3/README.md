# codec-ac3

[![Arduino Library](https://img.shields.io/badge/Arduino-Library-blue.svg)](https://www.arduino.cc/reference/en/libraries/)
[![Build with CMake](https://img.shields.io/badge/Build-CMake-064F8C.svg?logo=cmake)](https://cmake.org/)
[![ESP-IDF Component](https://img.shields.io/badge/ESP--IDF-Component-blue.svg?logo=espressif)](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/build-system.html#components)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

A header-only, dependency-free AC-3 (Dolby Digital / ATSC A/52) audio
decoder for Arduino, targeting resource-constrained microcontrollers such
as the ESP32 and RP2040. Decodes correctly on both; real-time decode
speed currently varies by platform - see [Decoding Speed](#decoding-speed).

## Features

- Decodes all AC-3 channel configurations (acmod 0-7) plus LFE, up to 5.1,
  as full discrete PCM in the bitstream's own channel order (see
  [AC3Decoder.h](src/AC3Decoder.h)). Optional downmixing to mono or
  stereo is available via `setOutputChannels()` (see below).
- Implements channel coupling, stereo rematrixing, dynamic range control,
  and both long (512-point) and short (2x256-point, block-switched) IMDCT.
- `float`-only throughout (`ac3::sample_t`) - no `double` anywhere in the
  library (verified with `-Wdouble-promotion`), since many
  microcontrollers (e.g. the RP2040) have no double-precision FPU and pay
  a large speed/flash-size penalty for it. The `AC3Decoder` facade
  converts to interleaved 16-bit PCM for you.
- Not implemented: E-AC-3 (Dolby Digital Plus, bsid > 8); the rare
  "halfrate" professional-production bsid 9-11 variant is only partially
  exercised.

## Usage

```cpp
#include "AC3Decoder.h"

AC3Decoder decoder;

void onPcm(const int16_t* pcm, int frameCount, int channels, int sampleRate,
           void* ref) {
  // pcm: frameCount * channels interleaved 16-bit samples
  // channel order: [L, C, R, SL, SR] trimmed to the coded acmod, then
  // LFE last if present (NOT standard WAV 5.1 order - reorder if needed)
}

void setup() {
  decoder.setPCMCallback(onPcm);
  decoder.begin();
}

void loop() {
  uint8_t buf[512];
  size_t n = readAc3BytesFromSomewhere(buf, sizeof(buf));
  if (n > 0) decoder.write(buf, n);  // any chunk size; sync frames are located internally
}
```

See [examples/simple-decode](examples/simple-decode) for a full sketch,
and [examples/benchmark](examples/benchmark) for a sketch that measures
decode speed against real-time playback - flash it to a board to check
whether that board is fast enough.

### Output sample type

`AC3Decoder` is a type alias for `AC3DecoderT<int16_t>`. The output
sample type is a template parameter, resolved at compile time - pick
whichever matches your output stage:

```cpp
AC3DecoderT<int16_t>        decoder;  // == AC3Decoder
AC3DecoderT<int32_t>        decoder;
AC3DecoderT<ac3::int24_t>   decoder;  // packed 3-byte PCM24 (see AC3Config.h)
AC3DecoderT<float>          decoder;  // raw decoder output, unclamped/unscaled
```

The callback's `pcm` parameter type follows automatically
(`const int32_t*`, `const ac3::int24_t*`, `const float*`, ...). `int16_t`/
`int32_t`/`ac3::int24_t` samples are scaled to that type's full range and
clamped to it; `float` is passed through as decoded (no scaling/clamping),
so a full-scale signal is nominally in [-1.0, 1.0] but transients can
briefly exceed that.

### Downmixing

By default `AC3Decoder` outputs the full discrete channel set the stream
was coded with (up to 5.1, in bitstream channel order). Call
`setOutputChannels(1)` or `setOutputChannels(2)` before/after `begin()`
to downmix every source configuration (mono, stereo, 3.0, 2.1, 3.1, 2.2,
5.1, dual-mono) to mono or stereo respectively, using the standard AC-3
center/surround downmix equations (driven by the stream's own cmixlev/
surmixlev values) with matching headroom normalization. `channels()`
always reports what will actually be delivered. LFE is not part of the
standard downmix and is dropped when downmixing (route it separately if
you need it). Pass `0` (the default) to go back to full discrete output.

```cpp
decoder.setOutputChannels(2);  // always deliver stereo, downmixing 5.1/3.0/etc. as needed
```

The same mixing logic is available standalone as `ac3::AC3Downmix::mix()`
(see [src/decoder/AC3Downmix.h](src/decoder/AC3Downmix.h)) if you're
using the lower-level `ac3::AC3Decoder` API directly.

Requesting a downmix doesn't just save a mixing step afterward - it saves
real decode work. Since the IMDCT (the dominant cost of decoding, see
"Decoding Speed" below) is linear, mixing the spectral coefficients of
up to 5 full-bandwidth channels down to 1-2 *before* the IMDCT and
running only that many transforms gives the identical result to running
one IMDCT per discrete channel and mixing the PCM afterward, for a
fraction of the work: on desktop, decoding this library's 96-second 5.1
test clip in full discrete (6 channels) took 0.474s; requesting stereo
took 0.320s (-32%) and mono 0.282s (-40%). This is exactly the lever
that makes downmixed real-time playback reachable on RP2040 where full
discrete 5.1 isn't (see below).

### Lower-level API

`AC3Decoder` is a thin convenience wrapper (byte-stream buffering + int16
conversion) around the core engine in
[src/decoder/AC3Decoder.h](src/decoder/AC3Decoder.h) (`ac3::AC3Decoder`),
which works directly with `float` PCM and complete frame buffers if you
want more control (e.g. to integrate with an existing audio pipeline):

```cpp
ac3::AC3Decoder dec;
dec.reset();
int frameSize = dec.parseHeader(buf, available);  // 0 if invalid/incomplete
if (frameSize > 0 && available >= frameSize) {
  int ch = dec.channelCount();
  float* chans[6]; float storage[6][256];
  for (int c = 0; c < ch; c++) chans[c] = storage[c];
  for (int b = 0; b < 6; b++) dec.decodeBlock(chans);  // 256 samples/channel/block
}
```

### Memory

Decode state (`AC3Decoder`/`ac3::AC3Decoder`) lives in the objects
themselves - declare them `static`/global (as in the examples) rather
than as local variables: `sizeof(AC3Decoder)` is ~25KB (`ac3::AC3Decoder`
alone is ~17KB), and putting that on the stack of a small task will
overflow it.

Per-call stack usage is small and bounded (well under 1KB at `-O2`): the
facade's ~11KB-per-frame scratch space (per-channel PCM, downmix
scratch, interleaved output) never lives on the stack. By default
`AC3Decoder` heap-allocates that scratch buffer in `begin()` and frees
it in `end()`, so it costs nothing while the decoder is idle/not yet
started; `begin()` returns `false` if that allocation fails. Call
`setScratchBuffer()` before `begin()` to avoid heap use entirely -
supply your own buffer instead (e.g. a `static` variable, a specific
memory region, or one buffer shared across several `AC3Decoder`
instances that never decode concurrently); `AC3Decoder` never frees a
buffer you supplied this way:

```cpp
static AC3Decoder::Scratch myScratch;  // AC3Decoder::scratchBufferSize() bytes
decoder.setScratchBuffer(&myScratch);
decoder.begin();  // no heap allocation, since a scratch buffer is already set
```

## Decoding Speed

Real-time factor = decode wall time / audio duration (below 1.0 = faster
than real time), measured with [examples/benchmark](examples/benchmark)
on real 5.1 AC-3 content.

| Platform | Clock | Output | Real-time factor | Margin |
|---|---|---|---|---|
| Desktop (x86_64, native build, `-O2`) | 1.7-3.3 GHz | 5.1 discrete | ~0.012x | ~85x faster than real time |
| ESP32 (Arduino core) | 240 MHz | 5.1 discrete | 0.272x | 3.7x faster than real time |
| ESP32-S3 (Arduino core) | 240 MHz | 5.1 discrete | 0.224x | 4.5x faster than real time |
| ESP32-P4 (Arduino core) | 360 MHz | 5.1 discrete | 0.129x | 7.8x faster than real time |
| Pico 2 / RP2350 (Arduino-Pico core, default clock) | 150 MHz | 5.1 discrete | 0.302x | 3.3x faster than real time |
| RP2040 (Arduino-Pico core, default clock) | 133 MHz | 5.1 discrete | 3.230x | 3.2x too slow |
| RP2040 (`menu.freq=200`)* | 200 MHz | 5.1 discrete | 1.938x | 1.9x too slow |
| RP2040 (`menu.freq=200`)* | 200 MHz | stereo (downmix) | 1.061x | 1.1x too slow |
| RP2040 (`menu.freq=276`)* | 276 MHz | stereo (downmix) | 0.768x | 1.3x faster than real time |

RP2040 has no hardware FPU: (*) with fixed point optimizations:  FPU-less targets ([AC3Fpu.h](src/decoder/AC3Fpu.h)) get a fixed-point IMDCT instead of `float` (unaffected elsewhere), plus
`setOutputChannels()` downmixing *before* the IMDCT instead of after -
both real decode-time savings, not just measurement tricks, which is
what gets RP2040 to real time for stereo/mono output. Full discrete 5.1
stays out of reach on RP2040 for now. Pico 2's RP2350 has a hardware
FPU (Cortex-M33), so it takes the plain-`float` path and reaches real
time even at full discrete 5.1, at stock clock.

## Running the tests

The tests build as a native (non-Arduino) CMake project:

```sh
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

- `tests/unit` - fast, self-contained checks (bitstream reader, frame
  header parsing, silent/malformed-input robustness, downmix identities);
  no external dependencies.
- `tests/decode-sample` - decodes [samples/sample-1.ac3](samples/sample-1.ac3)
  and, if `ffmpeg` is available on `PATH`, cross-checks the output
  (discrete and downmixed) against ffmpeg's own AC-3 decode; otherwise it
  falls back to structural/sanity checks only.

## License

The code in this repository is original and licensed under the MIT
License (see [LICENSE](LICENSE)). The bitstream syntax and the numeric
tables it implements (quantization levels, bit-allocation tables, window
function, etc.) are defined by the public ATSC A/52 standard and are
therefore necessarily identical across independent, interoperable
decoders.
