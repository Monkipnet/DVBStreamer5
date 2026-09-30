# Stage 9 - native transcoding, RTSP, RTMP and CMAF

Stage 9 restores the media features that were intentionally disabled when
GStreamer was removed, without introducing FFmpeg or another media framework.

Implemented:

- Native MPEG-TS elementary demux (PAT/PMT/PES/PTS/DTS) shared by protocols and transcoder.
- RTSP client input and embedded RTSP/RTP server output.
- RTMP/RTMPS client input and publish output with native handshake, chunks, AMF0 and FLV packetization.
- HLS fMP4/CMAF input/output, `EXT-X-MAP`, `.m4s` and SAMPLE-AES/cbcs.
- MPEG-TS HLS SAMPLE-AES encrypt/decrypt.
- Native transcoder pipeline: MPEG-TS -> decode -> I420/PCM processing -> encode -> native MPEG-TS mux.
- Locked project-local codec backends: OpenH264 2.6.0, libde265 1.1.3,
  Kvazaar 2.3.2, FDK-AAC 2.0.3 and PL_MPEG, plus existing TwoLAME MP2 encoder.
- Production build has no GStreamer, FFmpeg/libav or system codec runtime requirement.

Validation in the development sandbox:

- Full DVBStreamer5 target links successfully with the new Stage 9 modules.
- CTest: media_core, native_hls, native_srt, native_protocols,
  native_transcoder and mp2_encoder_tests all pass (6/6).
- `native_protocols` performs native RTSP loopback, RTMP publish/play loopback,
  MPEG-TS SAMPLE-AES roundtrip, CMAF `init.mp4 + .m4s -> MPEG-TS` roundtrip
  and encrypted CMAF/cbcs roundtrip.
- `native_transcoder` performs real H.264 encode -> MPEG-TS -> decode -> resize ->
  encode -> MPEG-TS -> decode and verifies the requested output geometry.

The development sandbox validation was performed with a temporary integration
harness because external codec source archives could not be downloaded there.
That fallback is not present in the Stage 9 source tree: production CMake
requires vendored SRT 1.5.7 and project-local static codec libraries. Run
`install_deps.sh` on Ubuntu before the production build to vendor/build all
locked sources.
