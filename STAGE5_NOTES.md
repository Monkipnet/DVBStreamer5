# Stage 5 — Native SRT

- No GStreamer.
- No `libsrt-dev` build dependency and no link-time `libsrt` dependency.
- Stage 5 originally loaded a system SRT runtime dynamically; Stage 6 supersedes this with an embedded SRT 1.5 payload loaded from memfd.
- Caller and listener input/output.
- MPEG-TS message payload default 1316 bytes.
- TSBPD/live/message API, latency, rcvlatency, peerlatency, buffers, FC, passphrase/PBKEYLEN and streamid.
- Reconnect loops and bounded output queue.
- Listener peers participate in subscriber filtering/session monitoring.
- VPS/VDS profile is reused.
