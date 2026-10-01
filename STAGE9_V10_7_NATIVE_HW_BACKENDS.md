# Stage 9 V10.7 — direct hardware video backends

This revision keeps the V10.4 packet-framer fix, V10.5 CBR budget, V10.6
realtime Kvazaar profile and the V10.7 asynchronous transcoder queues.

Hardware encoder selection is now part of the native codec factory:

- `auto`: NVENC -> Intel QSV/VAAPI -> CPU.
- `nvenc`: NVIDIA Video Codec SDK (`NvEncodeAPI`) loaded directly. The CUDA
  driver and NVENC runtime are opened with `dlopen`; no FFmpeg/libav/GStreamer.
- `qsv`: Intel oneVPL hardware encode. On Linux the loader is constrained to
  the VAAPI acceleration mode, so QSV uses the Intel VAAPI GPU path directly
  through the vendor API.
- `vaapi`: selects the same Intel oneVPL/VAAPI hardware path. This keeps one
  tested Intel encoder implementation while exposing the Linux VAAPI choice.
- `cpu`: OpenH264 for H.264 and Kvazaar for HEVC.

The hardware SDKs are optional at build time. CPU-only builds still compile and
run. CMake reports whether oneVPL and nvEncodeAPI headers were found.

Install Intel prerequisites with:

    sudo ./scripts/install_native_hw_backends.sh

For NVIDIA install the proprietary driver and Video Codec SDK headers
(`nvEncodeAPI.h`); DVBStreamer5 does not link to FFmpeg/GStreamer/libav.
