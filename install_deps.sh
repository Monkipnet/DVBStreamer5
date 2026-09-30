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
SRT_RUNTIME=""
for pkg in libsrt1.5-gnutls libsrt1.4-gnutls libsrt1.5-openssl libsrt1.4-openssl; do
  if apt-cache show "$pkg" >/dev/null 2>&1; then SRT_RUNTIME="$pkg"; break; fi
done
[[ -n "$SRT_RUNTIME" ]] || { echo "No compatible SRT runtime package found in APT." >&2; exit 1; }

"${APT_GET[@]}" install -y --no-install-recommends \
  build-essential cmake nodejs pkg-config \
  libpcsclite-dev pcscd pcsc-tools libccid \
  libssl-dev ca-certificates "$SRT_RUNTIME"
"${APT_GET[@]}" clean

echo "Dependencies installed."
echo "Build: cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --parallel --target DVBStreamer5"
