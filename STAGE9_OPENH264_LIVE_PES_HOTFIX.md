# Stage 9 V5 — OpenH264 live PES synchronization hotfix

Observed with a real HTTP MPEG-TS input after V4:

- transport stays ONLINE;
- AAC decodes and plays;
- VLC detects an H.264 elementary stream but reports 0 decoded video frames.

Root cause fixed here: the V2/V4 OpenH264 recovery gate stopped feeding the decoder until a single demux sample simultaneously satisfied the local SPS/PPS/IDR conditions. MPEG-TS PES boundaries are not required to coincide with H.264 access-unit or parameter-set boundaries. On real streams this can starve OpenH264 forever while audio continues and the output mux is mostly CBR stuffing.

V5 behavior:

- every H.264 PES payload is passed to OpenH264, including while unsynchronized;
- dsNoParamSets and other live/recovery statuses remain non-fatal;
- SPS/PPS are still cached when visible;
- when an IDR is seen and cached SPS/PPS are available but absent from that chunk, the IDR is primed with the cached parameter sets;
- the decoder is allowed to maintain its own parser state across successive calls rather than being starved by the application-level gate.

No FFmpeg or GStreamer is introduced.
