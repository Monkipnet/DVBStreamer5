# Stage 9 V8.5 — Kvazaar chunk-chain + PTS/DTS fix

Fixes HEVC output where VLC saw HEVC but decoded only a couple of blocks and displayed 0 frames.

Root cause:
- Kvazaar `kvz_data_chunk` nodes are fragments of one complete encoded access unit, not guaranteed NAL boundaries.
- The previous wrapper prepended an Annex-B start code to each chunk, corrupting NAL units split across chunks.
- Output timestamps were taken from `src` and DTS was forced equal to PTS, which is wrong when Kvazaar reorders frames.

Changes:
- concatenate Kvazaar chunks verbatim, matching the reference integration pattern;
- validate the accumulated byte count against `len`;
- add at most one leading Annex-B start code if the whole AU lacks one;
- inspect the complete AU for VPS/SPS/PPS/IRAP;
- use reconstructed/output picture PTS and DTS returned by Kvazaar;
- retain cached VPS/SPS/PPS priming for random-access AUs.
