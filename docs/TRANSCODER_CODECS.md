# MPEG-2 video and MP2 encoder investigation

## Current status

MPEG-2 video encoding has been canceled and is out of scope. Existing video
transcoding remains on the established GStreamer H.264 path. MP2 is exposed as
an audio codec and is wired through the in-process GStreamer transcoder. The
isolated `gst-launch-1.0` shared transcoder does not load the application-owned
element; `StreamManager` routes MP2 streams through the in-process path, and
direct attempts to start MP2 in the child process fail explicitly.

The in-process MP2 route converts decoded audio to 48 kHz stereo S16LE PCM,
encodes MPEG-1 Layer II with the vendored TwoLAME codec, parses the elementary
stream, and hands layer-2 caps to the existing MPEG-TS mux.

## Upstream implementation and licensing findings

The upstream GStreamer encoder elements are wrappers around separate codec
libraries; their wrapper sources do not contain the codec algorithms.

### MP2: vendored codec and standalone adapter

- Wrapper: [`gsttwolamemp2enc.c`](https://github.com/GStreamer/gst-plugins-good/blob/master/ext/twolame/gsttwolamemp2enc.c)
- Build definition: [`ext/twolame/meson.build`](https://github.com/GStreamer/gst-plugins-good/blob/master/ext/twolame/meson.build)
- The wrapper is a `GstAudioEncoder` element and requires the external TwoLAME
  library (`twolame >= 0.3.10`). It cannot be vendored alone to provide an
  encoder.
- Vendored codec: TwoLAME 0.4.0, commit
  `bec4069996479aa1aa9d9e7fa32c33135b3a2047`. The source is under
  `third_party/twolame/`; `COPYING`, `AUTHORS`, `README.md`, `NEWS.md`, per-file
  notices, and `UPSTREAM_REVISION` are preserved. CMake builds only the
  upstream `libtwolame` source list as the static target
  `dvbstreamer5_twolame`.
- The wrapper source is LGPL-2.0-or-later. TwoLAME's upstream `COPYING` is
  LGPL-2.1. The static library is linked in-tree; no runtime TwoLAME library
  is required. Distributions must also honor LGPL-2.1 static-linking terms,
  including providing the required relinkable application object files (or
  another compliant relinking mechanism) alongside the complete notices and
  corresponding library source.
- TwoLAME upstream: <https://github.com/njh/twolame>

The DVBStreamer5-owned `dvbstreamer5::media::Mp2Encoder` adapter in
`src/media/Mp2Encoder.h` accepts interleaved signed 16-bit PCM and appends
encoded bytes to a caller-owned vector. Its currently supported input profile
is mono or stereo at 32, 44.1, or 48 kHz, with fixed MPEG-1 Layer II bitrates
from the TwoLAME table (32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256,
320, or 384 kbit/s). Stereo selects joint-stereo mode; mono selects mono mode.
Input can arrive in arbitrary sample chunks; `finish()` flushes the final
partial frame. The adapter configures TwoLAME using
`twolame_init`/`twolame_set_num_channels`/`twolame_set_in_samplerate`/
`twolame_set_out_samplerate`/`twolame_set_bitrate`/`twolame_set_mode`/
`twolame_init_params`, sends PCM through `twolame_encode_buffer_interleaved`,
and drains with `twolame_encode_flush`.

`mp2_encoder_tests` exercises deterministic encode output, validates MPEG-1
Layer II frame headers and sizes, checks chunk-boundary invariance, and rejects
an unsupported channel layout. It does not use GStreamer or a system HTTP library.
It is available as an independent CMake project under `tests/mp2_encoder/` or
through the `DVBSTREAMER5_BUILD_MP2_ENCODER_TESTS` root option. The standalone
test intentionally does not create runtime GStreamer elements; MPEG-TS mux
integration remains part of the application path, not a test dependency.

### MPEG-2 video

- Wrapper: [`ext/mpeg2enc`](https://github.com/GStreamer/gst-plugins-bad/tree/master/ext/mpeg2enc)
- Build definition: [`ext/mpeg2enc/meson.build`](https://github.com/GStreamer/gst-plugins-bad/blob/master/ext/mpeg2enc/meson.build)
- The upstream wrapper is several C++ files built against both `mjpegtools
  >= 2.0.0` and its `mpeg2encpp` library. The upstream build notes that
  mjpegtools' API changes frequently and that its version is not exposed in a
  header. The wrapper is LGPL-2.0-or-later; the MPEG-2 encoder implementation
  in mjpegtools is GPL-2.0-or-later.
- Consequently, copying only the GStreamer wrapper would not provide the
  algorithm and would leave a runtime codec-library dependency. A statically
  vendored encoder needs a pinned mjpegtools source revision and its complete
  source dependency closure, with the applicable GPL notices and corresponding
  source obligations preserved.
- mjpegtools upstream: <https://sourceforge.net/projects/mjpeg/>

## Transcoder routing after Stage 2

The standalone `gst-launch-1.0` transcoder route is removed. The in-process
transcoder still uses GStreamer for parsing, decoding, raw video/audio
conversion and the currently selected H.264/H.265/AAC/MP3 encoder plugins, but
its transport-stream output is now application-owned.

`NativeMpegTsMux` receives parsed encoded access units through `appsink` and
builds the final SPTS in native C++ code. It generates PAT, PMT and SDT tables,
PES headers, 90 kHz PTS/DTS, PCR, continuity counters and optional CBR NULL
stuffing. The resulting aligned 188-byte packets are exposed through `appsrc`.
Consequently the transcoder core no longer requires `mpegtsmux`, `tsparse` or
`udpsink`. UDP output continues through the existing `StableUdpOutput`; HTTP
and SRT use the native mux timestamps directly, and transcoded HLS is segmented
from the native SPTS without a second demux/remux cycle.

MP2 remains application-owned through the vendored TwoLAME encoder. The native
mux advertises MPEG-1 Layer II/III with PMT stream type `0x03`, H.264 with
`0x1b`, HEVC with `0x24`, MPEG-2 video with `0x02`, and ADTS AAC with `0x0f`.
AC-3/E-AC-3 passthrough is carried as private stream type `0x06` with the DVB
AC-3/E-AC-3 registration descriptor.

The standalone media-core test validates native PAT/PMT CRCs, SID/PID mapping,
H.264/AAC PMT stream types, PES framing, PCR generation and CBR NULL stuffing.
A real live-stream integration test is still required for driver/codec-specific
GStreamer encoder behavior and end-to-end hardware playback.

No MPEG-2 video encoder work is planned.

## Native DVB scan, service selection, and CA

The DVB scan/signal path uses `LinuxDvbInput` directly: tune the Linux kernel
DVB frontend, read the DVR tap, and feed the existing PSI scanner. For a
selected service, the native UDP relay can use the MPEG-TS remapper to form a
single-program output. When a conditional-access client is configured, the
relay captures full TS so ECM/EMM packets are available, filters to the selected
program, then supplies aligned 77-packet batches to the existing in-place CA
backend before output. Network CA backends still require the application CA
plugin; this does not add a decoder or expose control words.

This route is restricted to Linux DVB selected-service streams with UDP VBR,
UDP CBR, or RTP outputs. It cannot share a frontend already owned by the
legacy shared-DVB GStreamer path; physical frontend behavior and real CAM/plugin
decryption still require hardware validation. Multi-section CA tables and PSI
sections spanning TS packets are not supported by the current native remapper.
