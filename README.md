# DVBStreamer5

DVBStreamer5 is a Linux x86_64 DVB/IPTV transport application with an in-project native MPEG-TS engine and web interface.

## Native media status — Stage 8

No GStreamer development headers, runtime libraries, plugins, `gst-launch`, or `gst-inspect` are used.

Available native paths:

- DVB-S/S2 input
- UDP/RTP MPEG-TS input and output
- HTTP/HTTPS MPEG-TS input and HTTP TS output/preview
- HLS MPEG-TS input and output
- HLS master/variant selection, Header/Query credentials and AES-128 input
- HLS live segmentation and DVR archive retention
- SRT caller/listener input and output with encrypted passphrase/PBKEYLEN support
- file-to-CBR TS path
- service/PID remap, PAT/PMT/SDT, PCR/PTS/DTS TS processing
- CBR pacing, MPTS and CA/OSCam transport integration

Still disabled pending native implementation: transcoding, RTSP, RTMP/YouTube and generated test pattern.

See `NATIVE_MEDIA_ENGINE.md` and `STAGE8_NOTES.md`.

## Build

```bash
./install_deps.sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel --target DVBStreamer5
```

Native test build:

```bash
cmake -S . -B build-test -DCMAKE_BUILD_TYPE=Release \
  -DDVBSTREAMER5_BUILD_MEDIA_CORE_TESTS=ON \
  -DDVBSTREAMER5_BUILD_MP2_ENCODER_TESTS=ON
cmake --build build-test --parallel 2 --target media_core_tests dvbstreamer5_native_hls_tests dvbstreamer5_native_srt_tests mp2_encoder_tests
ctest --test-dir build-test --output-on-failure
```

Runtime dependency audit:

```bash
./scripts/audit_runtime_deps.sh build/DVBStreamer5
```

## Source-built SRT 1.5.7 + OpenSSL EVP (Stage 8)

SRT no longer depends on a distro `libsrt` package at build time or runtime. The locked Haivision SRT 1.5.7 source tree is built as the upstream `srt_static` CMake target with encryption enabled and `USE_ENCLIB=openssl-evp`. `NativeSrtTransport` calls the linked SRT API directly.

Run `./install_deps.sh` once on a fresh checkout. It installs ordinary compiler/OpenSSL dependencies and, if `third_party/srt` has not yet been committed, runs `scripts/vendor_srt_source.sh` to download the pinned 1.5.7 orig source archive, verify SHA-256, and populate `third_party/srt`. Commit that source tree with the repository to make later builds network-independent.

The final executable has no external GStreamer, `libsrt`, GnuTLS or Nettle dependency. Caller/listener, TSBPD, live/message API, latency/buffers/FC, encrypted passphrase/PBKEYLEN, streamid, reconnect and subscriber filtering remain available.
