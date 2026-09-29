#!/usr/bin/env bash
set -euo pipefail

SRC="${1:?vendored source directory is required}"
OUT="${2:?output directory is required}"
WORK="$OUT/src"
BUILD="$OUT/cmake-build"
BIN_OUT="$OUT/oscam-mini"

for cmd in cmake gcc make pkg-config; do
  command -v "$cmd" >/dev/null || { echo "Missing build dependency: $cmd" >&2; exit 1; }
done

if ! pkg-config --exists openssl; then
  echo "OSCam-mini requires OpenSSL development files (libssl-dev)." >&2
  echo "Install: sudo apt-get install libssl-dev" >&2
  exit 6
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
# Enable the card-system handlers shipped by this OSCam snapshot.  Only Newcamd
# remains a network listener; enabling readers does not grant new access rights.
./config.sh --enable MODULE_NEWCAMD readers CARDREADER_PHOENIX

printf '\nEnabled OSCam-mini modules:\n'
./config.sh --show-enabled all

# Build via OSCam's CMakeLists instead of relying on the upstream root Makefile.
# This also avoids failures when an archive was unpacked through Windows and file
# permissions or the Makefile were lost.
cmake -S "$WORK" -B "$BUILD" \
  -DCMAKE_BUILD_TYPE=Release \
  -DHAVE_PCSC=1 \
  -DCS_CONFDIR=/opt/TVStreamer5/oscam-mini/config
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
echo "OSCam-mini built: $BIN_OUT"
