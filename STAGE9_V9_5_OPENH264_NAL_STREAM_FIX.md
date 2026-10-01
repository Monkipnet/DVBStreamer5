# Stage 9 V9.5 — OpenH264 NAL-stream decoder fix

- H.264 MPEG-TS demux no longer guesses AVC access-unit boundaries.
- PES payload is kept byte-exact and passed to the OpenH264 wrapper.
- OpenH264 wrapper performs Annex-B framing across PES boundaries and feeds complete NAL units through DecodeFrame2 (slice-level API).
- Live join waits for cached SPS/PPS plus an IDR before enabling decode.
- Decoded frame PTS uses SBufferInfo::uiOutYuvTimeStamp when available.
- OpenH264 encoder timestamps are normalized to a zero-based timeline for RC while MPEG-TS PTS remains unchanged.
