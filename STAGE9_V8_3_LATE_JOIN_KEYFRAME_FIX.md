# Stage 9 V8.3 — late-join H.264/H.265 random-access fix

Problem reproduced on a long-running native test-pattern/transcoder stream:
- VLC connects after the stream is already running;
- PMT exposes H.264/H.265 and AAC, but video stays blank/gray;
- restarting the DVBStreamer5 tile while VLC remains connected makes video appear immediately.

This proves the receiver was missing the encoder's startup random-access point and parameter sets.

V8.3 changes:
- OpenH264: force an IDR on frame 0 and approximately every 2 seconds with `ForceIntraFrame(true)`.
- H.264: cached SPS/PPS are still prepended to later IDR access units when absent.
- Kvazaar: configure `period` to approximately 2 seconds and `vps-period=1`.
- H.265: cached VPS/SPS/PPS are still prepended to IRAP access units when absent.

Result: a receiver may connect after the stream has already been running and should recover video within about 2 seconds without restarting the stream.

No FFmpeg/GStreamer/libav runtime dependency is added.
