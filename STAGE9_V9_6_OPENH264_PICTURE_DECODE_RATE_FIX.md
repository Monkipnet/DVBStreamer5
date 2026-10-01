# Stage 9 V9.6 — OpenH264 picture decode + bitrate accounting

Changes:
- Replaces V9.5 per-NAL DecodeFrame2 feeding with whole-picture DecodeFrameNoDelay feeding.
- Picture boundaries are detected in the continuous Annex-B stream at a new first slice, SPS/PPS after VCL, or repeated AUD, matching the OpenH264 console decoder model.
- Keeps PES payload byte-exact; no SPS/PPS/POC rewriting in the TS demuxer.
- Live join still waits for a complete IDR picture with cached SPS/PPS before synchronization.
- Adds NATIVE AVC PIC DIAG counters.
- Bitrate In is shown as a 5-second rolling average of raw HTTP input bytes.
- Payload Out excludes MPEG-TS null PID 0x1fff packets; CBR Out remains the physical paced output bitrate.

No FFmpeg, GStreamer, or libav dependency is added.
