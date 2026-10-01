# Stage 9 V10.6 — H.265 realtime CPU profile

This update changes only the Kvazaar H.265 encoder configuration.

- Keeps the working V10.4 separate PacketFramer input/output fix.
- Keeps the V10.5 CBR video-budget fix.
- Does not change the H.264 decoder/encoder path.
- Applies Kvazaar `preset=ultrafast` for live transcoding.
- Caps Kvazaar worker threads at 4 instead of `threads=auto`.
- Sets `owf=1` instead of `owf=auto`.
- Logs the active profile as `NATIVE H265 REALTIME PROFILE ...`.

The goal is to reduce CPU usage for one SD H.265 stream while preserving the
existing native codec/mux/SRT architecture.  No FFmpeg, GStreamer or libav
runtime dependency is introduced.
