# Stage 3 — native-only build/runtime

This release removes the legacy multimedia framework from both build-time and
runtime paths.

Verified in this source snapshot:

- CMake configures without media-framework development packages.
- `DVBStreamer5` builds successfully.
- `media_core_tests` pass.
- `mp2_encoder_tests` pass.
- runtime dependency audit passes.
- the executable has no `libgst*` dependency.
- no `Gst*` source files remain in the project tree.
- the executable starts, creates the default configuration, starts the HTTP
  worker pool and listens on port 9000.

Native active paths in Stage 3:

- Linux DVB-S/S2
- UDP/RTP MPEG-TS
- HTTP/HTTPS single-request MPEG-TS input
- local TS file input (CBR output path)
- native MPEG-TS remap/mux and pacing
- browser MPEG-TS preview
- MPTS
- CA transport hook

Temporarily disabled until later native stages:

- transcoding
- HLS
- SRT
- RTSP
- RTMP/YouTube
- test-pattern generation
