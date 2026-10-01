# Stage 9 V9.0 — live AVC open-GOP join

The LVM source is AVC Main 720x576p25 and uses B pictures. Diagnostic ffprobe
recovers after an initial missing-PPS interval and reports an I/B/B/P cadence.
A live broadcast stream may use non-IDR intra pictures/open GOPs, so waiting only
for nal_unit_type=5 can starve the decoder.

Changes:
- classify AVC I/SI slices from the first two Exp-Golomb slice-header fields;
- allow live OpenH264 synchronization on IDR or a non-IDR intra picture once
  SPS and PPS are cached;
- prepend cached SPS/PPS when absent from the join access unit;
- do not throw synchronization away for dsRefLost/dsRefListNullPtrs alone;
  those may occur briefly for leading B pictures in an open GOP;
- hard resync remains for missing parameter sets, bitstream errors and
  dependency-layer loss.

No FFmpeg/GStreamer/libav dependency is added.
