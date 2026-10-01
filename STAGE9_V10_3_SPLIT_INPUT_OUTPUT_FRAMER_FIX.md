# Stage 9 V10.3 — split input/output MPEG-TS framer fix

Root cause found by V10.2 capture:

- the H.264 elementary stream captured inside DVBStreamer5 was corrupt;
- a direct H.264 copy from the same HTTP source decoded cleanly with external ffmpeg;
- the native relay reused one `PacketFramer` both for raw HTTP/source bytes and for transformed/transcoded output bytes.

HTTP callback chunks are not guaranteed to be divisible by 188. Therefore the input framer can retain a partial source TS packet in `pending_`. Feeding transcoder output through that same framer appends unrelated output bytes to the pending source fragment. On the next input iteration, the source stream is no longer byte-exact. This explains why the internal test pattern worked while live HTTP H.264 with B pictures was corrupted.

V10.3 uses two independent framers:

- `inputFramer` — file/HTTP/UDP/DVB source only;
- `transformedFramer` — post-transform/transcoder output only.

Both are reset on an HTTP reconnect boundary. No codec, mux, SRT, AAC, FFmpeg, GStreamer or libav dependency is added or changed.
