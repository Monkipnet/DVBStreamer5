#!/usr/bin/env bash
set -euo pipefail
binary="${1:-build/DVBStreamer5}"
[[ -f "$binary" ]] || { echo "Binary not found: $binary" >&2; exit 2; }
file "$binary"
missing="$(ldd "$binary" 2>/dev/null | grep 'not found' || true)"
[[ -z "$missing" ]] || { echo "$missing" >&2; exit 1; }

# Production Stage 9 must not dynamically depend on media frameworks, SRT or
# codec runtimes. SRT and codec algorithms are linked from vendored static
# sources; OpenSSL and the ordinary C/C++ system runtime remain dynamic.
if readelf -d "$binary" 2>/dev/null | grep -Eqi \
  'libgst|libav(codec|format|filter|util|device)|libsw(scale|resample)|libsrt|libgnutls|libnettle|libopenh264|libde265|libkvazaar|libfdk'; then
  echo "Unexpected external media/codec runtime dependency detected" >&2
  exit 1
fi
if ! grep -aFq 'built-in:SRT 1.5.7/OpenSSL-EVP (vendored source)' "$binary"; then
  echo "Bundled SRT 1.5.7/OpenSSL-EVP marker not found in binary" >&2
  exit 1
fi
if ! grep -aFq 'built-in OpenH264 2.6.0 static' "$binary"; then
  echo "Stage 9 static codec marker not found in binary" >&2
  exit 1
fi
if readelf -d "$binary" 2>/dev/null | grep -Eq 'libboost|libcurl|libjsoncpp|libdvbcsa'; then
  echo "Unexpected external dependency: libboost/libcurl/libjsoncpp/libdvbcsa" >&2
  exit 1
fi
echo "PASS: Stage 9 native runtime dependency audit"
