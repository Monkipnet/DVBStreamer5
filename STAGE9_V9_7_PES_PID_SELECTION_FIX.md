# Stage 9 V9.7 — live PES length + elementary PID selection

This fix targets two live-source failure modes that do not occur with the internal test pattern.

1. The native transcoder now selects exactly one input video PID and one input audio PID from the PMT and ignores additional elementary streams of the same kind. Previously every video sample was fed into a single decoder instance, so two H.264 PIDs could corrupt the reference-picture state.
2. NativeTsDemux now honors a non-zero MPEG-2 PES_packet_length and removes bytes beyond the declared PES packet before the elementary stream is sent to OpenH264/FDK-AAC. It logs limited `NATIVE PES TRIM` / `NATIVE PES TRUNCATED` diagnostics.

The already working output H.264/H.265 encoders, mux, SRT pacing and test-pattern path are unchanged.
