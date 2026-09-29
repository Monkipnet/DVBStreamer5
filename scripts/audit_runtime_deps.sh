#!/usr/bin/env bash
set -euo pipefail

binary="${1:-build/TVStreamer5}"
if [[ ! -f "$binary" ]]; then
  echo "Binary not found: $binary" >&2
  exit 2
fi

echo "Binary: $(readlink -f "$binary")"
file "$binary"
echo

echo "ELF interpreter and DT_NEEDED:"
if command -v readelf >/dev/null 2>&1; then
  readelf -l "$binary" | grep -E 'Requesting program interpreter' || true
  readelf -d "$binary" | grep -E '\(NEEDED\)|\(RPATH\)|\(RUNPATH\)' || true
else
  echo "readelf is unavailable"
fi
echo

echo "Resolved shared libraries:"
if command -v lddtree >/dev/null 2>&1; then
  lddtree "$binary"
else
  ldd "$binary"
fi
echo

missing="$(ldd "$binary" 2>/dev/null | grep 'not found' || true)"
if [[ -n "$missing" ]]; then
  echo "Missing libraries:" >&2
  printf '%s\n' "$missing" >&2
  exit 1
fi

if command -v gst-inspect-1.0 >/dev/null 2>&1; then
  echo "GStreamer registry:"
  echo "  scanner: ${GST_PLUGIN_SCANNER:-auto}"
  echo "  plugin path: ${GST_PLUGIN_PATH:-system default}"
  for element in h264parse h265parse x264enc x265enc nvh264enc nvh265enc \
      qsvh264enc qsvh265enc vah264enc vah265enc mpegtsmux hlssink; do
    if gst-inspect-1.0 "$element" >/dev/null 2>&1; then
      printf '  [ok]      %s\n' "$element"
    else
      printf '  [missing] %s\n' "$element"
    fi
  done
fi
