# Stage 9 V8.4 — H.265/Kvazaar live fix

Changes:
- Keep Kvazaar 2.3.2 native GOP/intra defaults instead of overriding period/vps-period via config_parse.
- Preserve cached VPS/SPS/PPS injection on every IRAP access unit for late-join decodability.
- Improve Kvazaar encoder_encode failure diagnostics with geometry and PTS.
- Test-pattern source now reports the actual downstream relay/transcoder error instead of generic "test pattern sink stopped accepting MPEG-TS".
- H.264 V8.3 periodic IDR logic remains unchanged.
- DVB UTF-8 remapping from V8.2 remains unchanged.
