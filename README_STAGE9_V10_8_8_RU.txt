Stage 9 V10.8.8 - VAAPI H.264 startup AU fix

Fixes:
- libva 2.20 VACodedBufferSegment::next void* compatibility.
- Direct VAAPI H.264 output normalized to Annex-B when needed.
- Generates matching SPS/PPS for the configured VAAPI profile/geometry.
- Repeats SPS/PPS on every IDR.
- Adds diagnostic:
  NATIVE VAAPI H264 startup bytes=... sps=1 pps=1 idr=1

Why:
NativeTranscoderPipeline waits for SPS + PPS + IDR before publishing the first
H.264 video access unit. Legacy Intel i965 may emit the IDR slice without
in-band SPS/PPS, leaving HLS/output waiting forever.
