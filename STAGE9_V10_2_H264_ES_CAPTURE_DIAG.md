# Stage 9 V10.2 — H.264 ES capture diagnostic

Purpose: capture the exact H.264 elementary-stream bytes delivered by NativeTsDemux to OpenH264, without changing normal media behavior.

Enable only for diagnostics:

```bash
DVBSTREAMER5_DUMP_H264_ES=/tmp/lvm-native.h264 ./DVBStreamer5
```

The capture is capped at 64 MiB. A sidecar CSV is written to `<path>.index.csv` with one line per selected H.264 sample (size/PTS/DTS/random-access flags).

No FFmpeg/GStreamer/libav dependency is added. External ffprobe/ffmpeg may be used only after capture to validate the dumped elementary stream.
