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
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

"${APT_GET[@]}" update
SRT_RUNTIME=""
if [[ ! -f "$ROOT_DIR/third_party/srt-embedded/libsrt-runtime.so" ]]; then
  for pkg in libsrt1.5-openssl libsrt1.5-gnutls; do
    if apt-cache show "$pkg" >/dev/null 2>&1; then SRT_RUNTIME="$pkg"; break; fi
  done
  if [[ -z "$SRT_RUNTIME" ]]; then
    echo "SRT 1.5 build-time payload not found." >&2
    echo "Install/provide an SRT 1.5 runtime and pass -DDVBSTREAMER5_SRT_RUNTIME=/path/to/libsrt-*.so.1.5," >&2
    echo "or place a target-compatible payload at third_party/srt-embedded/libsrt-runtime.so." >&2
    exit 1
  fi
fi

DEPS=(build-essential cmake nodejs pkg-config
      libpcsclite-dev pcscd pcsc-tools libccid
      libssl-dev ca-certificates binutils)
[[ -n "$SRT_RUNTIME" ]] && DEPS+=("$SRT_RUNTIME")
"${APT_GET[@]}" install -y --no-install-recommends "${DEPS[@]}"
"${APT_GET[@]}" clean

echo "Dependencies installed. CMake will embed the SRT 1.5 payload and OpenSSL-backed crypto shims directly into DVBStreamer5."
echo "Build: cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --parallel --target DVBStreamer5"
