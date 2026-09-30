#!/usr/bin/env bash
set -euo pipefail
binary="${1:-build/DVBStreamer5}"
[[ -f "$binary" ]] || { echo "Binary not found: $binary" >&2; exit 2; }
file "$binary"
missing="$(ldd "$binary" 2>/dev/null | grep 'not found' || true)"
[[ -z "$missing" ]] || { echo "$missing" >&2; exit 1; }
if readelf -d "$binary" 2>/dev/null | grep -Eqi 'libgst|libsrt'; then
  echo "Unexpected external media runtime dependency detected (GStreamer/SRT)" >&2
  exit 1
fi
if ! grep -aFq 'embedded:libsrt-1.5' "$binary"; then
  echo "Embedded SRT Stage 6 marker not found in binary" >&2
  exit 1
fi
if readelf -d "$binary" 2>/dev/null | grep -Eq 'libboost|libcurl|libjsoncpp|libdvbcsa'; then
  echo "Unexpected external dependency: libboost/libcurl/libjsoncpp/libdvbcsa" >&2
  exit 1
fi
echo "PASS: native runtime dependency audit"
