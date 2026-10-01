# Stage 9 V10.1 — OpenH264 direct-PES live decode fix

Scope: live AVC input decoding only. Audio/PES selection from V10.0 is preserved.

## Why
V10.0 diagnostics show one selected AVC PES sample per ~40 ms (about 25 fps), but
the internal OpenH264 stream splitter creates roughly two synthetic "pictures"
per input sample (for example: 100 video samples vs 200 parsed pictures). That
split destroys the B-picture reference chain and produces repeated
`Ref Picture for B-Slice is lost` with zero decoded frames.

## Change
`OpenH264Decoder::decode()` no longer re-splits the already framed H.264 PES
payload. Each byte-exact PES elementary payload is submitted once to the
existing IDR/SPS/PPS synchronization path and then to `DecodeFrameNoDelay()`.

The existing AVC bitstream mode (`VIDEO_BITSTREAM_AVC`), SPS/PPS cache, IDR gate,
error diagnostics, V10.0 audio/PES handling, H.264 encoder, Kvazaar H.265 encoder,
mux and SRT output are unchanged.

New diagnostic line:
`NATIVE AVC PES DIAG decodeCalls=... samples=... sps=... pps=... idr=... out=...`

Expected live LVM relationship after this fix:
- `NATIVE TRANSCODER VIDEO samples=100 ...`
- `NATIVE AVC PES DIAG ... samples=100 ...`
not the previous ~2:1 synthetic picture ratio.
