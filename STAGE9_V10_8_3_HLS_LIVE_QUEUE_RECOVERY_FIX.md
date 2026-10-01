# Stage 9 V10.8.3 — HLS live AV queue recovery

Fixes HLS/ABR streams going OFFLINE with `native AV mux scheduler audio queue overflow`.

The V10.8.1 strict A/V lead correctly prevents audio from running many seconds ahead
of video, but a temporarily slow video encoder can intentionally hold the audio queue.
V10.8.2 treated a full encoded mux queue as fatal.  With HLS ABR this is more likely
because several rendition encoders run at once.

V10.8.3 keeps the pipeline live: when a bounded encoded mux queue reaches its limit,
the oldest stale sample is discarded and the newest live sample is kept.  The stream
is no longer failed solely because an AV mux queue temporarily saturates.  Diagnostics
add `NATIVE AV MUX DROP ... keep_live=1` and `drop_video`/`drop_audio` counters.

No FFmpeg, GStreamer or libav production dependency is introduced.
