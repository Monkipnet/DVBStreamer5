# Stage 9 V8.7 — live IDR/IRAP decoder resync

Fixes real broadcast/live inputs that start in the middle of a GOP.

- H.264/OpenH264 now caches SPS/PPS but drops P/B access units until the first IDR.
- Before decoding that first IDR, OpenH264 reference state is reinitialized and cached SPS/PPS are prepended when absent.
- Recoverable ref/bitstream loss returns the decoder to the clean-IDR gate instead of continuously feeding broken B-slices.
- H.265/libde265 now uses the same policy: cache VPS/SPS/PPS, wait for IRAP, reset DPB, prepend missing parameter sets, then decode.
- This is intentionally downstream of the V8.6 PES->access-unit assembly; V8.7 assumes complete AUs and no longer needs to feed mid-GOP fragments to decoder parser state.

The fix targets the observed OpenH264 log storm: `Ref Picture for B-Slice is lost` on the live `lvm` source while native test pattern works.
