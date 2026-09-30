# DVBStreamer5

DVBStreamer5 is a C++17 television transport server with a native media engine.
The project has no dependency on an external multimedia framework or plugin
registry.

## Stage 3 native engine

Implemented natively:

- Linux DVB-S/S2 frontend control, scanning, signal/quality and service PID selection
- UDP MPEG-TS input/output
- RTP MPEG-TS input/output
- HTTP/HTTPS single-request MPEG-TS input
- local MPEG-TS file input for UDP CBR output
- MPEG-TS framing and continuity tracking
- PAT/PMT/SDT/PES generation and remapping
- PCR/PTS/DTS handling and CBR null-packet pacing
- browser MPEG-TS preview fan-out
- MPTS aggregation
- CA/OSCam transport processing
- embedded web UI and JSON API

Temporarily disabled while native implementations are added:

- video/audio transcoding
- HLS input/output
- SRT input/output
- RTSP input/output
- RTMP/YouTube input/output
- generated test pattern

## Build

Ubuntu/Debian:

```bash
./install_deps.sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel --target DVBStreamer5
```

Binary:

```text
build/DVBStreamer5
```

Run:

```bash
./build/DVBStreamer5
```

Web UI defaults to port 9000.

## Native tests

```bash
cmake -S . -B build-tests -DCMAKE_BUILD_TYPE=Release \
  -DDVBSTREAMER5_BUILD_MEDIA_CORE_TESTS=ON \
  -DDVBSTREAMER5_BUILD_MP2_ENCODER_TESTS=ON
cmake --build build-tests --parallel --target dvbstreamer5_media_core_tests dvbstreamer5_mp2_encoder_tests
ctest --test-dir build-tests --output-on-failure
```

## Runtime dependency audit

```bash
./scripts/audit_runtime_deps.sh build/DVBStreamer5
```

The audit fails if an obsolete multimedia-framework runtime library appears in
`DT_NEEDED`.
