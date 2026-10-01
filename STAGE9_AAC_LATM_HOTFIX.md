# Stage 9 live AAC/LATM hotfix

This update fixes the live transcoder failure reported as:

    FDK-AAC DecodeFrame failed: 5

Root cause in the Stage 9 tree:

* MPEG-TS PMT stream_type 0x0f (AAC/ADTS) and 0x11 (AAC/LATM) were both mapped to `ElementaryCodec::AacAdts`.
* The FDK decoder was therefore always opened with `TT_MP4_ADTS`.
* A real stream using PMT stream_type 0x11 was consequently fed to the wrong FDK transport parser.

Changes:

* adds `ElementaryCodec::AacLatm`;
* maps PMT stream_type 0x0f -> AAC ADTS and 0x11 -> AAC LATM separately;
* preserves stream_type 0x11 when LATM is copied through the native MPEG-TS mux;
* opens FDK-AAC with `TT_MP4_ADTS` for ADTS and `TT_MP4_LOAS` for LATM;
* if a 0x11 stream repeatedly cannot synchronize as LOAS, retries with `TT_MP4_LATM_MCP1`;
* treats `AAC_DEC_TRANSPORT_SYNC_ERROR` as a live-stream resynchronization condition instead of taking the service OFFLINE;
* treats isolated `AAC_DEC_UNKNOWN` (numeric value 5) as recoverable and restarts the AAC transport decoder after repeated occurrences;
* correctly retains bytes that `aacDecoder_Fill()` could not accept in one call instead of discarding them.

The prior Stage 9 HTTP reconnect and OpenH264 SPS/PPS/IDR hotfixes are retained in the full-source archive.

Validation performed in the sandbox:

* `NativeTsDemux.cpp`, `NativeMpegTsMux.cpp`, and `NativeTranscoderPipeline.cpp` compile with `-Wall -Wextra -Wpedantic`;
* the revised FDK decoder body passes an isolated C++17 syntax check against the FDK API surface used by this project;
* a native MPEG-TS regression harness confirms PMT stream_type 0x11 round-trips as `ElementaryCodec::AacLatm`.

A full production compile with the user's vendored FDK-AAC/OpenH264/libde265/Kvazaar libraries must still be run on the WSL tree.
