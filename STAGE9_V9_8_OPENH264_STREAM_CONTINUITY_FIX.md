# Stage 9 V9.8 — OpenH264 stream framing + TS continuity

V9.7 fixed the audio stutter by honoring PES_packet_length and selecting a single audio/video PID. The live LVM source is now measured at about 3.2–3.4 Mbit/s, while useful output remains audio-only because the H.264 decoder still reports many bitstream errors and produces no frames.

V9.8 changes only the live AVC input/decoder side:

- OpenH264 framing now follows the same picture-boundary counters used by Cisco's `h264dec` console decoder over one continuous Annex-B byte stream instead of rebuilding pictures NAL-by-NAL.
- The decoder is initialized in the console-style auto/default bitstream mode with decoder threading disabled/automatic single-stream behavior for this 720x576 Main-profile source.
- MPEG-TS continuity is tracked on each elementary PID. Duplicate TS packets are ignored. On a continuity gap/discontinuity, an incomplete PES is discarded and the demux waits for the next PUSI before resuming that PID.
- Runtime diagnostics: `NATIVE AVC STREAM DIAG`, `NATIVE TS CONTINUITY`, `NATIVE TS DUPLICATE`, `NATIVE AVC OUTPUT`.

No FFmpeg/GStreamer/libav dependency is added. H.264/H.265 encoders, mux, SRT output, remapping and the V9.7 PES-length/audio fix are unchanged.
