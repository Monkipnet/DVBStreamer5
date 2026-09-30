#!/usr/bin/env bash
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST_DIR="$ROOT_DIR/third_party/srt-embedded"
DEST="$DEST_DIR/libsrt-runtime.so"
mkdir -p "$DEST_DIR"

source_path="${1:-}"
if [[ -z "$source_path" ]]; then
  source_path="$(ldconfig -p 2>/dev/null | awk '
    /libsrt-openssl\.so\.1\.5/ {print $NF; found=1; exit}
    /libsrt-gnutls\.so\.1\.5/ {fallback=$NF}
    END {if (!found && fallback) print fallback}
  ')"
fi
if [[ -z "$source_path" || ! -e "$source_path" ]]; then
  echo "SRT 1.5 runtime not found. Install libsrt1.5-openssl/libsrt1.5-gnutls (or pass a path)." >&2
  exit 1
fi
source_path="$(readlink -f "$source_path")"
cp "$source_path" "$DEST"
chmod 0644 "$DEST"
sha="$(sha256sum "$DEST" | awk '{print $1}')"
package="$(dpkg-query -S "$source_path" 2>/dev/null | head -1 | cut -d: -f1 || true)"
version=""
if [[ -n "$package" ]]; then version="$(dpkg-query -W -f='${Version}' "$package" 2>/dev/null || true)"; fi
{
  echo "Embedded SRT payload source: $source_path"
  echo "Package: ${package:-unknown}"
  echo "Package version: ${version:-unknown}"
  echo "SHA-256: $sha"
  echo "Architecture: $(uname -m)"
} > "$DEST_DIR/RUNTIME_INFO.txt"
echo "Vendored SRT runtime: $DEST"
cat "$DEST_DIR/RUNTIME_INFO.txt"
