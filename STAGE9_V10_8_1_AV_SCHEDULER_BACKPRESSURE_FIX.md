# Stage 9 V10.8.1 — AV scheduler/backpressure fix

Fixes two regressions seen in the V10.8 live test:

1. The mux scheduler allowed a queue depth >= 2 to bypass the 150 ms A/V lead limit. Audio could therefore run seconds ahead of video.
2. The compressed-video input queue was only 24 samples, causing repeated GOP resyncs during normal HTTP burst delivery or a briefly slower encoder.

Changes:
- strict 150 ms peer lead once both audio and video clocks exist;
- no queue-depth bypass of the lead guard;
- video compressed queue 24 -> 128 samples;
- audio input queue 256 -> 512 samples;
- startup peer wait 250 -> 500 ms;
- scheduler wait uses a fresh short deadline once both peers exist, preventing busy loops on old queued timestamps.

This does not change codec bitstreams or add FFmpeg/GStreamer/libav dependencies.
