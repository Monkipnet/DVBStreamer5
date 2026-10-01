# Stage 9 V9.4 — source/payload/CBR bitrate + live pipeline diagnostics

This patch does not change the already-working H.264/H.265 output encoders.

Changes:
- HTTP input bitrate is counted at the HTTP body callback, before queueing, TS framing, remap, demux or transcoding.
- Added MPEG-TS payload-output counter before CBR null stuffing and before fan-out to physical outputs.
- Stream tile now separates `Bitrate In`, `Payload Out`, and `CBR Out`.
- Added `NATIVE RATE` diagnostic every 5 seconds for transcoded streams.
- Added video/audio sample counters and PTS/DTS diagnostics in the native transcoder.
- Existing OpenH264 AVC diagnostics from V9.1 remain enabled.

Expected diagnostic form:

    NATIVE RATE stream=lvm source_kbps=3400 payload_kbps=... cbr_kbps=6000 cc_delta=0
    NATIVE TRANSCODER VIDEO samples=100 decoded=0 bytes=... pts=... dts=... random=...
    NATIVE TRANSCODER AUDIO samples=100 decoded=... bytes=... pts=...
    NATIVE AVC DIAG ... out=...

No FFmpeg/GStreamer/libav dependency is added.
