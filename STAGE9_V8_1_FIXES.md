# Stage 9 V8.1 fixes

- OpenH264 2.6.0 compatibility: removed non-existent SDecodingParam::eOutputColorFormat assignment. Decode output is already I420 via SBufferInfo planes.
- Kvazaar rate-control initialization: when target_bitrate > 0, set rc_algorithm = KVZ_LAMBDA before encoder_open, matching the supported Kvazaar embedding flow.
- Native test-pattern generator implemented. `test_pattern`, `input_mode=test`, `test://bars`, `testsrc://bars`, and `bars://hd` now use an internally generated 1280x720/25 H.264 + AAC MPEG-TS source.
- Test pattern uses SMPTE-like color bars plus a moving white marker, silent AAC at 48 kHz stereo, native codec backends and NativeMpegTsMux only.
- Test-pattern input goes through the same normal relay/transcoder/output path, so it can be used to diagnose H.264/H.265 transcoding without an external source.
- No FFmpeg, libav*, or GStreamer dependency was added.
