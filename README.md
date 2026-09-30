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

## Embedded SRT (Stage 6)

SRT input/output no longer depends on a separately installed `libsrt*.so` at runtime. During the source build, an SRT 1.5 shared object is converted by `objcopy` into a read-only ELF object and linked into `DVBStreamer5`. `NativeSrtTransport` exposes it through an anonymous Linux `memfd` and loads `/proc/self/fd/<n>`, so the deployed program has no `DT_NEEDED` entry for `libsrt` and no SRT `.so` file beside the executable.

`install_deps.sh` uses the distro SRT 1.5 package only as a build-time source for the embedded payload; it does not copy the library into the Git working tree. The binary installer does not install SRT. Use `-DDVBSTREAMER5_SRT_RUNTIME=/path/to/libsrt-*.so.1.5` to embed a specific SRT 1.5 build. Caller/listener, TSBPD, live/message API, latency/buffers/FC, passphrase/PBKEYLEN, streamid, reconnect and subscriber filtering remain unchanged.
