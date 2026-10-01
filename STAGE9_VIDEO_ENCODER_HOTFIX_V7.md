# Stage 9 V7 — H.264/H.265 encoder bitstream hotfix

Fixes native transcoder encoder output packaging.

## H.264 / OpenH264
- Normalizes every encoded NAL to Annex-B (`00 00 00 01`).
- Collects all `SFrameBSInfo` layers/NALs without assuming the buffer already carries start codes.
- Caches SPS/PPS from encoder output.
- Prepends cached SPS/PPS to IDR/I access units when the encoder omits them.

## H.265 / Kvazaar
- Normalizes every output chunk to Annex-B.
- Parses output NAL types instead of relying only on `kvz_frame_info::nal_unit_type`.
- Caches VPS/SPS/PPS.
- Detects IRAP NALs (types 16..23) as random access frames.
- Prepends VPS/SPS/PPS to IRAP access units when missing.

The project remains native-only: no FFmpeg/GStreamer/libav dependency is added.
