# Stage 9 V9.1 — AVC open-GOP recovery + diagnostics

Changes:
- Keep OpenH264 synchronized across recoverable `dsBitstreamError`/reference-loss reports after a live open-GOP join.
- Force a new random-access join only for `dsNoParamSets` or `dsDepLayerLost`.
- Add periodic decoder diagnostics: calls, SPS/PPS, IDR/intra, sync state, OpenH264 state, reference loss, missing params, bitstream errors, output frames.
- Log the first decoded raw AVC frame and every 100th frame.

This patch does not alter the already-working H.264/H.265 output encoders, mux, SRT output, or DVB UTF-8 remapping.
