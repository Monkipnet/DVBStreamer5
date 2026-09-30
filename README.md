# DVBStreamer5

DVBStreamer5 is a Linux DVB/IPTV transport and transcoding application with an
in-project native media engine and web interface.

## Native media status - Stage 9

No GStreamer or FFmpeg/libav media framework is used.

Available paths include DVB-S/S2, UDP/RTP, HTTP/HTTPS, HLS MPEG-TS, HLS
fMP4/CMAF, SRT, RTSP and RTMP/RTMPS. HLS supports AES-128 and SAMPLE-AES;
CMAF supports `EXT-X-MAP`, `init.mp4`, `.m4s` and SAMPLE-AES/cbcs.

Video/audio transcoding is again in-process and native to the DVBStreamer5
pipeline. Codec algorithms are supplied by individually pinned project-local
libraries (OpenH264, libde265, Kvazaar, FDK-AAC and PL_MPEG), not by a media
framework. TwoLAME remains the in-tree MP2 encoder.

See `NATIVE_MEDIA_ENGINE.md` and `STAGE9_NOTES.md` for supported formats and
current codec limits.

## Build

On a fresh Ubuntu checkout run once:

```bash
./install_deps.sh
```

This vendors SRT 1.5.7 and the locked codec source snapshots and builds the
static codec prefix. No `libsrt-dev`, FFmpeg/libav or GStreamer package is
required.

Because CMake `try_compile/configure_file` can fail on WSL DrvFS/NTFS, keep the
build directory in the Linux filesystem when the source tree is on `/mnt/c` or
`/mnt/d`:

```bash
cmake -S /mnt/d/PROJECTS/DVBStreamer5 -B "$HOME/DVBStreamer5-build" \
  -DCMAKE_BUILD_TYPE=Release \
  -DDVBSTREAMER5_BUILD_MEDIA_CORE_TESTS=ON \
  -DDVBSTREAMER5_BUILD_MP2_ENCODER_TESTS=ON
cmake --build "$HOME/DVBStreamer5-build" --parallel 2 --target \
  DVBStreamer5 media_core_tests dvbstreamer5_native_hls_tests \
  dvbstreamer5_native_srt_tests dvbstreamer5_native_protocol_tests \
  dvbstreamer5_native_transcoder_tests mp2_encoder_tests
ctest --test-dir "$HOME/DVBStreamer5-build" --output-on-failure
```

Runtime dependency audit:

```bash
./scripts/audit_runtime_deps.sh "$HOME/DVBStreamer5-build/DVBStreamer5"
```

## Locked native codec sources

`scripts/vendor_native_codecs.sh` uses the commits recorded in
`third_party/NATIVE_CODEC_SOURCE_LOCK.txt`. `scripts/build_native_codecs.sh`
builds static codec libraries into `third_party/native-codecs-prefix`.

There is no runtime-codec or system-SRT fallback in Stage 9. The normal build
requires the locked vendored sources and project-local static codec prefix.
