# Stage 9 V8.6 — live MPEG-TS access-unit assembly

## Fixed

Real broadcast/HTTP MPEG-TS does not guarantee that one PES equals one video frame.
The synthetic test-pattern source happened to produce frame-aligned PES, which is why
H.264/H.265 transcoding worked there but not on the live `lvm` input.

V8.6 changes `NativeTsDemux` so H.264/H.265 PES payloads are reassembled into
complete Annex-B access units before they are passed to OpenH264 or libde265.

Boundary detection supports:
- H.264 AUD (NAL type 9)
- H.264 new-picture detection using `first_mb_in_slice == 0`
- H.265 AUD (NAL type 35)
- H.265 `first_slice_segment_in_pic_flag`
- NAL units split across PES boundaries

This also fixes the misuse of `de265_push_end_of_frame()` on arbitrary PES chunks.
The decoder now receives one complete picture before end-of-frame is signaled.

AAC handling is left on the existing streaming FDK-AAC path; its decoder already
buffers transport bytes across PES boundaries and runs an internally continuous
90 kHz clock after first lock.

No FFmpeg, libav* or GStreamer dependency was added.
