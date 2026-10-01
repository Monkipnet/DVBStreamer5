# Stage 9 V9.9 — live MPEG-TS continuity tolerance

Observed on the LVM HTTP source after V9.8:

- source bitrate is healthy (~3.2–3.5 Mbit/s),
- audio is stable after V9.7 PES length/PID selection,
- the source emits very frequent continuity-counter jumps on the selected media PIDs,
- V9.8 treated every CC gap as packet loss, cleared the current PES and waited for the next PUSI,
- as a result OpenH264 never saw SPS/PPS/IDR (`sps=0 pps=0 idr=0`).

V9.9 changes NativeTsDemux continuity handling:

1. A plain continuity-counter gap is diagnostic only (`SOFT-GAP`, action=preserve).
   It no longer clears an in-progress PES.
2. An explicit MPEG-TS adaptation-field `discontinuity_indicator` remains authoritative
   and still resets the current PES / resynchronizes on PUSI.
3. A repeated continuity counter is not automatically discarded.  Only a byte-identical
   188-byte TS packet is suppressed as an exact duplicate.
4. Same-CC packets with different payload are preserved and reported diagnostically.
5. PES_packet_length and PUSI remain the primary payload integrity/boundary checks.

This is deliberately limited to the live input demux path.  The already working test-pattern
H.264/H.265 encoders, AAC fix, output mux, CBR pacing and SRT output are unchanged.
