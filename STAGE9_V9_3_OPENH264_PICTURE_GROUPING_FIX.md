# Stage 9 V9.3 — OpenH264-compatible live AVC picture grouping

## Problem
The live LVM HTTP MPEG-TS source reaches the transcoder correctly and repeatedly carries SPS/PPS/IDR, but OpenH264 reports `dsBitstreamError`/lost B-picture references and outputs zero raw frames. The native test pattern works through both H.264 and H.265 output encoders, so the defect is in the live AVC input framing path.

## Fix
`NativeTsDemux::queueVideoSample()` now groups Annex-B AVC exactly in the style used by OpenH264's own `h264dec` console decoder:

- preserve elementary-stream bytes exactly across PES boundaries;
- discard only a live-join prefix before the first valid Annex-B start code;
- retain complete multi-slice pictures;
- start a new picture when a later VCL NAL has `first_mb_in_slice == 0`;
- also split at repeated SPS/PPS after VCL or the second AUD;
- do not rewrite SPS/PPS, slice headers, or NAL payload bytes.

This replaces the custom SPS/PPS/frame_num/POC boundary parser introduced in V8.9, which could mis-group a broadcast Main-profile B-frame stream even though the byte stream itself was valid.

## Scope
Only live H.264 demux/framing is changed. HTTP input diagnostics, decoder diagnostics, test-pattern generation, H.264 output encoding, Kvazaar H.265 output encoding, muxing, SRT, DVB UTF-8 remapping, and audio code are unchanged.
