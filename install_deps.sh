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
DEPS=(build-essential cmake nodejs pkg-config
      libpcsclite-dev pcscd pcsc-tools libccid
      libssl-dev ca-certificates curl binutils)
"${APT_GET[@]}" install -y --no-install-recommends "${DEPS[@]}"
"${APT_GET[@]}" clean

"$ROOT_DIR/scripts/vendor_srt_source.sh"
if [[ ${EUID} -eq 0 && -n "${SUDO_USER:-}" && "${SUDO_USER}" != root ]]; then
  chown -R "$SUDO_USER":"$(id -gn "$SUDO_USER")" "$ROOT_DIR/third_party/srt" 2>/dev/null || true
fi

echo "Dependencies installed. SRT 1.5.7 source is vendored and will be built statically with OpenSSL EVP."
echo "No libsrt-dev/libsrt runtime package is required."
echo "Build: cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --parallel 2 --target DVBStreamer5"
