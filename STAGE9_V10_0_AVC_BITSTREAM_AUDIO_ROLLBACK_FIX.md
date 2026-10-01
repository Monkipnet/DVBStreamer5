# Stage 9 V10.0 — AVC bitstream type + audio-stability rollback

This fix addresses two findings from the LVM live-source tests.

1. OpenH264 decoder initialization used `VIDEO_BITSTREAM_DEFAULT`. In OpenH264,
   the default is the SVC bitstream type, while the LVM input is ordinary AVC
   (H.264 Main profile with B pictures). The decoder is now initialized with
   `VIDEO_BITSTREAM_AVC`.

2. V9.8/V9.9 continuity-counter filtering changed live packet handling and the
   audio began stuttering again. The demux packet path is restored to the V9.7
   behavior that produced stable audio. PES_packet_length trimming and the
   single selected audio/video PID logic from V9.7 remain in place.

No FFmpeg/GStreamer/libav dependency is added.
