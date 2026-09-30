# DVBStreamer5

DVBStreamer5 is a Linux x86_64 DVB/IPTV transport application with an in-project native MPEG-TS engine and web interface.

## Native media status — Stage 4

No GStreamer development headers, runtime libraries, plugins, `gst-launch`, or `gst-inspect` are used.

Available native paths:

- DVB-S/S2 input
- UDP/RTP MPEG-TS input and output
- HTTP/HTTPS MPEG-TS input and HTTP TS output/preview
- HLS MPEG-TS input and output
- HLS master/variant selection, Header/Query credentials and AES-128 input
- HLS live segmentation and DVR archive retention
- file-to-CBR TS path
- service/PID remap, PAT/PMT/SDT, PCR/PTS/DTS TS processing
- CBR pacing, MPTS and CA/OSCam transport integration

Still disabled pending native implementation: transcoding, RTSP, RTMP/YouTube and generated test pattern.

See `NATIVE_MEDIA_ENGINE.md` and `STAGE4_NOTES.md`.

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
cmake --build build-test --parallel --target media_core_tests dvbstreamer5_native_hls_tests mp2_encoder_tests
ctest --test-dir build-test --output-on-failure
```

Runtime dependency audit:

```bash
./scripts/audit_runtime_deps.sh build/DVBStreamer5
```

## Native SRT (Stage 5)

SRT input and output are implemented without GStreamer and without `libsrt-dev`. DVBStreamer5 uses a small runtime ABI loader for the standard SRT runtime (`libsrt 1.4+`), so CMake and compilation do not require SRT headers or link-time SRT libraries. Caller/listener modes, live/message API, TSBPD, latency, receive/peer latency, buffers, flight window, passphrase/PBKEYLEN, streamid, reconnect and subscriber filtering are supported. Ubuntu 22.04 and 24.04 runtime package names are detected by `install_deps.sh`.
