#!/usr/bin/env bash
set -euo pipefail

SRC_INPUT="${1:?vendored source directory is required}"
OUT_INPUT="${2:?output directory is required}"
DVBCSA_INCLUDE_INPUT="${3:?vendored libdvbcsa include directory is required}"
DVBCSA_LIBRARY_INPUT="${4:?vendored ECM-aware libdvbcsa library is required}"

# Canonicalize paths before changing into the copied OSCam work tree below.
# Callers commonly pass repository-relative paths such as
#   third_party/oscam-mini build-10.8.179/oscam-mini
# and keeping those paths relative after `cd "$WORK"` made CMake resolve them
# underneath WORK again (for example .../src/build-10.8.179/.../src).
SRC="$(cd "$SRC_INPUT" && pwd)"
mkdir -p "$OUT_INPUT"
OUT="$(cd "$OUT_INPUT" && pwd)"
DVBCSA_INCLUDE="$(cd "$DVBCSA_INCLUDE_INPUT" && pwd)"
DVBCSA_LIBRARY="$(cd "$(dirname "$DVBCSA_LIBRARY_INPUT")" && pwd)/$(basename "$DVBCSA_LIBRARY_INPUT")"
WORK="$OUT/src"
BUILD="$OUT/cmake-build"
BIN_OUT="$OUT/oscam-mini"

for cmd in cmake gcc make pkg-config nm; do
  command -v "$cmd" >/dev/null || { echo "Missing build dependency: $cmd" >&2; exit 1; }
done

if ! pkg-config --exists openssl; then
  echo "OSCam-mini requires OpenSSL development files (libssl-dev)." >&2
  echo "Install: sudo apt-get install libssl-dev" >&2
  exit 6
fi

if [[ ! -f "$DVBCSA_INCLUDE/dvbcsa/dvbcsa.h" || ! -f "$DVBCSA_LIBRARY" ]]; then
  echo "OSCam-mini requires the vendored DVBStreamer5 ECM-aware libdvbcsa build." >&2
  echo "Header:  $DVBCSA_INCLUDE/dvbcsa/dvbcsa.h" >&2
  echo "Library: $DVBCSA_LIBRARY" >&2
  exit 7
fi
if ! grep -q 'DVBCSA_KEY_ECM' "$DVBCSA_INCLUDE/dvbcsa/dvbcsa.h"; then
  echo "Vendored libdvbcsa header is missing DVBCSA_KEY_ECM." >&2
  exit 8
fi
if ! nm -g --defined-only "$DVBCSA_LIBRARY" 2>/dev/null | grep -q 'dvbcsa_bs_key_set_ecm'; then
  echo "Vendored libdvbcsa library is missing dvbcsa_bs_key_set_ecm()." >&2
  exit 9
fi

if [[ ! -f "$SRC/config.sh" || ! -f "$SRC/CMakeLists.txt" ]]; then
  echo "ERROR: OSCam source is incomplete in: $SRC" >&2
  echo "Expected at least config.sh and CMakeLists.txt." >&2
  echo "Commit the complete third_party/oscam-mini tree to the repository." >&2
  exit 2
fi

# PC/SC smart-card reader support is mandatory for this build; fail loudly rather
# than distributing a binary without OMNIKEY support.
if ! pkg-config --exists libpcsclite || [[ ! -f /usr/include/PCSC/wintypes.h ]]; then
  echo "OSCam-mini PC/SC requires libpcsclite-dev (and pcscd at runtime)." >&2
  echo "Install: sudo apt-get install libpcsclite-dev pcscd pcsc-tools" >&2
  exit 4
fi

rm -rf "$WORK" "$BUILD"
mkdir -p "$OUT"
cp -a "$SRC" "$WORK"
mkdir -p "$WORK/Distribution" "$WORK/webif"
# The vendored source may be checked out or extracted on Windows with CRLF
# shell scripts. Normalize every script executed by the OSCam CMake/WebIf
# build; a CRLF shebang otherwise fails in WSL with a misleading "not found".
for script in \
  "$WORK/config.sh" \
  "$WORK/webif/pages_mkdep" \
  "$WORK/webif/pages_index_check"; do
  sed -i 's/\r$//' "$script"
  chmod +x "$script"
done
# pages_gen parses file names and preprocessor conditions directly from this
# index, so a trailing CR corrupts every entry and leaves pages.c incomplete.
sed -i 's/\r$//' "$WORK/webif/pages_index.txt"

cd "$WORK"
./config.sh --disable all
# Enable all network proxy/client modules supported by this OSCam snapshot plus
# the existing card-system handlers. Runtime access still depends on explicit
# reader/listener configuration; enabling a module does not grant access rights.
./config.sh --enable WITH_SSL MODULE_NEWCAMD MODULE_CAMD33 MODULE_CAMD35 MODULE_CAMD35_TCP MODULE_CCCAM MODULE_CCCSHARE MODULE_GBOX MODULE_RADEGAST MODULE_SCAM MODULE_GHTTP MODULE_STREAMRELAY readers CARDREADER_PHOENIX

printf '\nEnabled OSCam-mini modules:\n'
./config.sh --show-enabled all

# Build via OSCam's CMakeLists instead of relying on the upstream root Makefile.
# This also avoids failures when an archive was unpacked through Windows and file
# permissions or the Makefile were lost.
cmake -S "$WORK" -B "$BUILD" \
  -DCMAKE_BUILD_TYPE=Release \
  -DHAVE_PCSC=1 \
  -DDVBSTREAMER5_DVBCSA_INCLUDE_DIR="$DVBCSA_INCLUDE" \
  -DDVBSTREAMER5_DVBCSA_LIBRARY="$DVBCSA_LIBRARY" \
  -DCS_CONFDIR=/opt/DVBStreamer5/oscam-mini/config
# Detect any unexpected configure fallback to non-PC/SC mode.
if ! grep -Eq '(^CONFIG_CARDREADER_PCSC=y$|^USE_PCSC[=: ]|^WITH_PCSC[=: ]|^HAVE_PCSC(:INTERNAL|:UNINITIALIZED|:BOOL)?=1$)' \
     "$WORK/config.mak" "$BUILD/config.mak" "$BUILD/CMakeCache.txt" 2>/dev/null; then
  echo "OSCam-mini PC/SC support not confirmed after CMake configuration" >&2
  exit 5
fi
cmake --build "$BUILD" --target oscam -j"$(nproc)"

if [[ ! -x "$BUILD/oscam" ]]; then
  echo "ERROR: OSCam binary was not produced at $BUILD/oscam" >&2
  exit 3
fi

install -m0755 "$BUILD/oscam" "$BIN_OUT"

if ldd "$BIN_OUT" 2>/dev/null | grep -q 'libdvbcsa'; then
  echo "ERROR: OSCam-mini unexpectedly links system/dynamic libdvbcsa." >&2
  ldd "$BIN_OUT" | grep 'libdvbcsa' >&2 || true
  exit 10
fi
if ! "$BIN_OUT" -V 2>&1 | grep -q 'Streamrelay:.*yes'; then
  echo "ERROR: OSCam-mini was built without StreamRelay." >&2
  exit 11
fi

echo "OSCam-mini built with vendored ECM-aware static libdvbcsa: $BIN_OUT"
