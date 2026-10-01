#!/usr/bin/env bash
set -euo pipefail

if [[ ${EUID:-$(id -u)} -ne 0 ]]; then
  echo "Run as root: sudo $0" >&2
  exit 1
fi

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
  libva2 libva-drm2 libva-dev vainfo \
  libvpl2 libvpl-dev \
  pkg-config

cat <<'MSG'
Intel QSV/VAAPI build prerequisites installed.

NVIDIA NVENC is intentionally not pulled from an unofficial binary package.
Install the NVIDIA proprietary driver plus Video Codec SDK headers so that
nvEncodeAPI.h is visible to CMake (common path: /usr/include/ffnvcodec).
The runtime libraries libcuda.so.1 and libnvidia-encode.so.1 are loaded
dynamically by DVBStreamer5; FFmpeg/GStreamer/libav are not used.

Reconfigure CMake after installing hardware SDK headers:
  cmake -S . -B build-test
  cmake --build build-test --parallel 2 --target DVBStreamer5
MSG
