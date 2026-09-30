#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="${DVBSTREAMER5_SRT_SOURCE_DIR:-$ROOT_DIR/third_party/srt}"
VERSION="1.5.7"
ARCHIVE_NAME="srt_${VERSION}.orig.tar.gz"
SHA256="017cd1e437ef2073a4dd10ddf7b55e86bc3d6ebac0393d13bd22f6a57055d32b"

if [[ -f "$DEST/CMakeLists.txt" ]] && grep -Eq 'set[[:space:]]*\([[:space:]]*SRT_VERSION[[:space:]]+1\.5\.7[[:space:]]*\)' "$DEST/CMakeLists.txt"; then
  echo "Vendored SRT $VERSION source already present: $DEST"
  exit 0
fi

command -v curl >/dev/null 2>&1 || { echo "curl is required to vendor SRT source" >&2; exit 1; }
command -v sha256sum >/dev/null 2>&1 || { echo "sha256sum is required" >&2; exit 1; }
command -v tar >/dev/null 2>&1 || { echo "tar is required" >&2; exit 1; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
ARCHIVE="$TMP/$ARCHIVE_NAME"
URLS=(
  "https://deb.debian.org/debian/pool/main/s/srt/$ARCHIVE_NAME"
  "https://ftp.ussg.iu.edu/linux/debian/pool/main/s/srt/$ARCHIVE_NAME"
  "https://mirror.hoster.kz/debian/pool/main/s/srt/$ARCHIVE_NAME"
)

ok=0
for url in "${URLS[@]}"; do
  echo "Downloading SRT $VERSION source: $url"
  if curl --fail --location --retry 3 --connect-timeout 15 --output "$ARCHIVE" "$url"; then
    if printf '%s  %s\n' "$SHA256" "$ARCHIVE" | sha256sum -c -; then
      ok=1
      break
    fi
    echo "Checksum mismatch from $url" >&2
    rm -f "$ARCHIVE"
  fi
done
[[ "$ok" -eq 1 ]] || { echo "Unable to download verified SRT $VERSION source" >&2; exit 1; }

rm -rf "$DEST"
mkdir -p "$DEST"
tar -xzf "$ARCHIVE" --strip-components=1 -C "$DEST"

if ! grep -Eq 'set[[:space:]]*\([[:space:]]*SRT_VERSION[[:space:]]+1\.5\.7[[:space:]]*\)' "$DEST/CMakeLists.txt"; then
  echo "Downloaded archive is not SRT $VERSION" >&2
  rm -rf "$DEST"
  exit 1
fi

cat > "$DEST/DVBSTREAMER5_VENDOR_INFO.txt" <<INFO
Upstream: Haivision SRT
Version: $VERSION
Upstream tag: v$VERSION
Upstream release commit: 899348d8318eb9a3c5a5b6ec43c4a1114288773a
Source package: $ARCHIVE_NAME
SHA-256: $SHA256
License: MPL-2.0 (see LICENSE in this directory)
Crypto backend selected by DVBStreamer5 CMake: OpenSSL EVP
INFO

echo "Vendored SRT $VERSION source into: $DEST"
