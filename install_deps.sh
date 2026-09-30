#!/usr/bin/env bash
set -euo pipefail

if ! command -v apt-get >/dev/null 2>&1; then
  echo "This installer requires an apt-based Ubuntu/Debian system." >&2
  exit 1
fi

SUDO=()
if [[ ${EUID} -ne 0 ]]; then
  command -v sudo >/dev/null 2>&1 || { echo "Run as root or install sudo first." >&2; exit 1; }
  SUDO=(sudo)
fi
APT_GET=("${SUDO[@]}" apt-get)

"${APT_GET[@]}" update
"${APT_GET[@]}" install -y --no-install-recommends \
  build-essential cmake nodejs pkg-config \
  libpcsclite-dev pcscd pcsc-tools libccid \
  libssl-dev ca-certificates
"${APT_GET[@]}" clean

echo "Dependencies installed."
echo "Build: cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --parallel --target DVBStreamer5"
