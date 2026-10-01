# Stage 9 V10.8 — AV mux scheduler

This update keeps the V10.7/V10.7.1 asynchronous video/audio workers and native hardware backends, but removes direct MPEG-TS writes from those workers.

Encoded video and audio now enter independent bounded mux queues. A dedicated mux worker orders samples by DTS/PTS, limits one stream from running more than 150 ms ahead of the other, waits briefly for the peer stream at startup, and is the only worker that writes media samples into NativeMpegTsMux.

New diagnostics:

    NATIVE ASYNC TRANSCODER ... av_mux_scheduler=1 max_av_lead_ms=150
    NATIVE AV MUX vq=... aq=... video=... audio=... av_delta_ms=... late_video=... late_audio=...

The goal is stable A/V interleave for CPU H.264/H.265 and for native NVENC/QSV/VAAPI backends, especially when one encoder completes frames significantly faster than the other.

No FFmpeg, GStreamer, or libav production dependency is added.
