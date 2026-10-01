# Stage 9 V8.2

- Adds a transcoder startup gate: no H.264 video PES is published until an IDR AU containing SPS/PPS is available; HEVC waits for IRAP + VPS/SPS/PPS. This removes the first-start / manual-restart dependency.
- Resets startup-gate state on pipeline reset.
- Remapper SDT service/provider text now uses DVB UTF-8 selector 0x15 for Cyrillic/non-ASCII names instead of replacing bytes with `?`.
- Native MPEG-TS mux and MPTS SDT generation use the same DVB UTF-8 encoding, so remap, transcode/test-pattern and MPTS paths are consistent.
- No FFmpeg/GStreamer/libav dependency added.
