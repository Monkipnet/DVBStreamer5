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
      libssl-dev ca-certificates curl binutils nasm autoconf automake libtool)
"${APT_GET[@]}" install -y --no-install-recommends "${DEPS[@]}"
"${APT_GET[@]}" clean

# Detect GPU vendors and install native hardware codec dependencies/drivers.
bash "$ROOT_DIR/scripts/install_native_hw_backends.sh"

bash "$ROOT_DIR/scripts/vendor_srt_source.sh"
bash "$ROOT_DIR/scripts/vendor_native_codecs.sh"
bash "$ROOT_DIR/scripts/build_native_codecs.sh"
if [[ ${EUID} -eq 0 && -n "${SUDO_USER:-}" && "${SUDO_USER}" != root ]]; then
  chown -R "$SUDO_USER":"$(id -gn "$SUDO_USER")" "$ROOT_DIR/third_party/srt" 2>/dev/null || true
  chown -R "$SUDO_USER":"$(id -gn "$SUDO_USER")"     "$ROOT_DIR/third_party/openh264" "$ROOT_DIR/third_party/libde265" "$ROOT_DIR/third_party/kvazaar"     "$ROOT_DIR/third_party/fdk-aac" "$ROOT_DIR/third_party/pl_mpeg" "$ROOT_DIR/third_party/native-codecs-prefix" 2>/dev/null || true
fi

echo "Dependencies installed. SRT 1.5.7 source is vendored and will be built statically with OpenSSL EVP."
echo "Pinned native codec sources are vendored and built into third_party/native-codecs-prefix as static libraries."
echo "No libsrt-dev, FFmpeg/libav, or GStreamer runtime is required."
echo "Native hardware APIs are enabled automatically when supported: Intel oneVPL/VAAPI, NVIDIA NVENC, generic VAAPI."
echo "WSL/NTFS note: keep the CMake build directory in the Linux filesystem."
echo 'Build example: cmake -S "$PWD" -B "$HOME/DVBStreamer5-build" -DCMAKE_BUILD_TYPE=Release && cmake --build "$HOME/DVBStreamer5-build" --parallel 2 --target DVBStreamer5' 
