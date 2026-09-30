# Stage 5 — Native SRT

- No GStreamer.
- No `libsrt-dev` build dependency and no link-time `libsrt` dependency.
- Runtime SRT ABI is loaded dynamically (`libsrt` 1.4+).
- Caller and listener input/output.
- MPEG-TS message payload default 1316 bytes.
- TSBPD/live/message API, latency, rcvlatency, peerlatency, buffers, FC, passphrase/PBKEYLEN and streamid.
- Reconnect loops and bounded output queue.
- Listener peers participate in subscriber filtering/session monitoring.
- VPS/VDS profile is reused.
