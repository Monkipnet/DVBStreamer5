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

## Embedded SRT + OpenSSL crypto shims (Stage 7)

SRT input/output remains embedded in the final `DVBStreamer5` ELF and has no external `libsrt*.so` runtime dependency. Stage 7 additionally removes the external GnuTLS/Nettle dependency from a GnuTLS-flavoured SRT payload: two tiny compatibility modules export only the seven ABI symbols used by SRT 1.5.x and implement RNG, AES-CTR and PBKDF2-HMAC-SHA1 through the same OpenSSL `libcrypto` already used by DVBStreamer5.

At runtime the GnuTLS-compatible shim (`SONAME libgnutls.so.30`) and Nettle-compatible shim (`SONAME libnettle.so.8`) are loaded from anonymous Linux `memfd` objects with `RTLD_GLOBAL` before the embedded SRT object. This satisfies the embedded SRT `DT_NEEDED` entries without loading system `libgnutls` or `libnettle`. The deployed executable therefore needs neither a separate SRT package nor GnuTLS/Nettle packages.

`install_deps.sh` uses a distro SRT 1.5 shared object only as a build-time payload and prefers the OpenSSL flavour when available. `-DDVBSTREAMER5_SRT_RUNTIME=/path/to/libsrt-*.so.1.5` still selects an explicit payload. Caller/listener, TSBPD, live/message API, latency/buffers/FC, encrypted passphrase/PBKEYLEN, streamid, reconnect and subscriber filtering remain unchanged.
