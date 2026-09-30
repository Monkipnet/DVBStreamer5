#!/usr/bin/env bash
set -euo pipefail
binary="${1:-build/DVBStreamer5}"
[[ -f "$binary" ]] || { echo "Binary not found: $binary" >&2; exit 2; }
file "$binary"
missing="$(ldd "$binary" 2>/dev/null | grep 'not found' || true)"
[[ -z "$missing" ]] || { echo "$missing" >&2; exit 1; }
if readelf -d "$binary" 2>/dev/null | grep -Eqi 'libgst|libsrt|libgnutls|libnettle'; then
  echo "Unexpected external media/crypto runtime dependency detected (GStreamer/SRT/GnuTLS/Nettle)" >&2
  exit 1
fi
if ! grep -aFq 'built-in:SRT 1.5.7/OpenSSL-EVP (vendored source)' "$binary"; then
  echo "Bundled SRT 1.5.7/OpenSSL-EVP Stage 8 marker not found in binary" >&2
  exit 1
fi
if readelf -d "$binary" 2>/dev/null | grep -Eq 'libboost|libcurl|libjsoncpp|libdvbcsa'; then
  echo "Unexpected external dependency: libboost/libcurl/libjsoncpp/libdvbcsa" >&2
  exit 1
fi
echo "PASS: native runtime dependency audit"
