# Stage 9 V8.8 — preserve live AVC/HEVC PES feed

The V8.6 AU splitter used a simplified slice-boundary heuristic. On a real
broadcast H.264 stream with B pictures / field coding it could split a reference
picture and OpenH264 then reported `Ref Picture for B-Slice is lost` continuously.

V8.8 leaves H.264/H.265 PES payloads intact and lets the native decoders parse
Annex-B. Late join remains protected by the V8.7 IDR/IRAP resync gate. Synthetic
test pattern encoding paths are unchanged.
