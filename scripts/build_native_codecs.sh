#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PREFIX="${DVBSTREAMER5_CODEC_PREFIX:-$ROOT/third_party/native-codecs-prefix}"
BUILD_ROOT="${DVBSTREAMER5_CODEC_BUILD_ROOT:-${TMPDIR:-/tmp}/dvbstreamer5-native-codecs-build}"
JOBS="${DVBSTREAMER5_CODEC_JOBS:-2}"

need_dir() { [[ -d "$1" ]] || { echo "Missing vendored codec source: $1" >&2; echo "Run ./scripts/vendor_native_codecs.sh first." >&2; exit 1; }; }
need_file() { [[ -f "$1" ]] || { echo "Native codec artifact missing: $1" >&2; exit 1; }; }
for d in openh264 ittiam-libavc libde265 kvazaar fdk-aac ittiam-libmpeg2 pl_mpeg; do need_dir "$ROOT/third_party/$d"; done

rm -rf "$BUILD_ROOT" "$PREFIX"
mkdir -p "$BUILD_ROOT" "$PREFIX/include" "$PREFIX/lib"

arch="$(uname -m)"
case "$arch" in
  x86_64|amd64) oh_arch=x86_64 ;;
  i386|i486|i586|i686) oh_arch=x86 ;;
  aarch64|arm64) oh_arch=arm64 ;;
  armv7l|armv7*) oh_arch=arm ;;
  *) oh_arch="$arch" ;;
esac

echo "[1/7] OpenH264 static"
make -C "$ROOT/third_party/openh264" -j"$JOBS" OS=linux ARCH="$oh_arch" BUILDTYPE=Release libopenh264.a
make -C "$ROOT/third_party/openh264" OS=linux ARCH="$oh_arch" BUILDTYPE=Release install-static PREFIX="$PREFIX"

echo "[2/7] Ittiam AVC decoder static"
# Ittiam libavc and libmpeg2 both export an upstream helper named
# check_app_out_buf_size. They are normally built as separate libraries, but
# DVBStreamer5 links both statically into one executable. Give the AVC copy a
# codec-specific name at compile time so the final link remains strict and we
# do not have to hide the collision with --allow-multiple-definition.
cmake -S "$ROOT/third_party/ittiam-libavc" -B "$BUILD_ROOT/ittiam-libavc" \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
  -DCMAKE_C_FLAGS="-Dcheck_app_out_buf_size=ih264d_check_app_out_buf_size" \
  -DENABLE_MVC=OFF -DENABLE_SVC=OFF -DENABLE_TESTS=OFF
cmake --build "$BUILD_ROOT/ittiam-libavc" --target libavcdec --parallel "$JOBS"
AVC_LIB="$(find "$BUILD_ROOT/ittiam-libavc" -type f \
  \( -name 'libavcdec.a' -o -name 'liblibavcdec.a' \) -print -quit)"
if [[ -z "$AVC_LIB" ]]; then
  echo "Ittiam AVC static library was not produced" >&2
  exit 1
fi
install -m 0644 "$AVC_LIB" "$PREFIX/lib/libavcdec.a"
install -d "$PREFIX/include/ittiam-avc"
find "$ROOT/third_party/ittiam-libavc/common" "$ROOT/third_party/ittiam-libavc/decoder" \
  -maxdepth 1 -type f -name '*.h' -exec install -m 0644 {} "$PREFIX/include/ittiam-avc/" \;

# Do not use grep -q on the producer side of a pipe while pipefail is enabled:
# grep may exit as soon as it finds a match, nm then receives SIGPIPE and the
# otherwise successful test becomes status 141. Read nm completely instead.
AVC_SYMBOLS="$(nm -g --defined-only "$PREFIX/lib/libavcdec.a" 2>/dev/null)"
if grep -E '[[:space:]]check_app_out_buf_size$' <<<"$AVC_SYMBOLS" >/dev/null; then
  echo "Ittiam AVC symbol isolation failed: check_app_out_buf_size is still exported" >&2
  exit 1
fi
if ! grep -E '[[:space:]]ih264d_check_app_out_buf_size$' <<<"$AVC_SYMBOLS" >/dev/null; then
  echo "Ittiam AVC symbol isolation failed: renamed helper is missing" >&2
  exit 1
fi

echo "[3/7] libde265 static"
cmake -S "$ROOT/third_party/libde265" -B "$BUILD_ROOT/libde265" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DBUILD_SHARED_LIBS=OFF -DENABLE_DECODER=ON -DENABLE_ENCODER=OFF \
  -DENABLE_SDL=OFF -DENABLE_SHERLOCK265=OFF -DENABLE_INTERNAL_DEVELOPMENT_TOOLS=OFF
cmake --build "$BUILD_ROOT/libde265" --parallel "$JOBS"
cmake --install "$BUILD_ROOT/libde265"

echo "[4/7] Kvazaar static"
KVAZAAR_CMAKE_VERSION="$(cmake --version | awk 'NR==1 {print $3}')"
KVAZAAR_MIN_VERSION="$(printf '%s\n' "3.25" "$KVAZAAR_CMAKE_VERSION" | sort -V | sed -n '1p')"
if [[ "$KVAZAAR_MIN_VERSION" == "3.25" ]]; then
  echo "Kvazaar: using CMake $KVAZAAR_CMAKE_VERSION"
  cmake -S "$ROOT/third_party/kvazaar" -B "$BUILD_ROOT/kvazaar" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTS=OFF -DBUILD_KVAZAAR_BINARY=OFF -DGIT_SUBMODULE=OFF -DUSE_CRYPTO=OFF
  cmake --build "$BUILD_ROOT/kvazaar" --parallel "$JOBS"
  cmake --install "$BUILD_ROOT/kvazaar"
else
  echo "Kvazaar: CMake $KVAZAAR_CMAKE_VERSION is older than 3.25; using upstream Autotools build"
  (
    cd "$ROOT/third_party/kvazaar"
    bash ./autogen.sh
  )
  mkdir -p "$BUILD_ROOT/kvazaar-autotools"
  (
    cd "$BUILD_ROOT/kvazaar-autotools"
    "$ROOT/third_party/kvazaar/configure" \
      --prefix="$PREFIX" \
      --disable-shared \
      --enable-static
    make -j"$JOBS"
    make install
  )
fi

echo "[5/7] FDK-AAC static"
cmake -S "$ROOT/third_party/fdk-aac" -B "$BUILD_ROOT/fdk-aac" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" -DBUILD_SHARED_LIBS=OFF
cmake --build "$BUILD_ROOT/fdk-aac" --parallel "$JOBS"
cmake --install "$BUILD_ROOT/fdk-aac"

echo "[6/7] Ittiam MPEG-2 decoder static"
cmake -S "$ROOT/third_party/ittiam-libmpeg2" -B "$BUILD_ROOT/ittiam-libmpeg2" \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF
cmake --build "$BUILD_ROOT/ittiam-libmpeg2" --target libmpeg2dec --parallel "$JOBS"
MPEG2_LIB="$(find "$BUILD_ROOT/ittiam-libmpeg2" -type f \
  \( -name 'libmpeg2dec.a' -o -name 'liblibmpeg2dec.a' \) -print -quit)"
if [[ -z "$MPEG2_LIB" ]]; then
  echo "Ittiam MPEG-2 static library was not produced" >&2
  exit 1
fi
install -m 0644 "$MPEG2_LIB" "$PREFIX/lib/libmpeg2dec.a"
install -d "$PREFIX/include/ittiam-mpeg2"
find "$ROOT/third_party/ittiam-libmpeg2/common" "$ROOT/third_party/ittiam-libmpeg2/decoder" \
  -maxdepth 1 -type f -name '*.h' -exec install -m 0644 {} "$PREFIX/include/ittiam-mpeg2/" \;

echo "[7/7] PL_MPEG header"
install -d "$PREFIX/include/pl_mpeg"
install -m 0644 "$ROOT/third_party/pl_mpeg/pl_mpeg.h" "$PREFIX/include/pl_mpeg/pl_mpeg.h"

echo "Validating native codec prefix"
for artifact in \
  "$PREFIX/include/wels/codec_api.h" \
  "$PREFIX/include/ittiam-avc/ih264d.h" \
  "$PREFIX/include/libde265/de265.h" \
  "$PREFIX/include/kvazaar.h" \
  "$PREFIX/include/fdk-aac/aacdecoder_lib.h" \
  "$PREFIX/include/fdk-aac/aacenc_lib.h" \
  "$PREFIX/include/ittiam-mpeg2/impeg2d.h" \
  "$PREFIX/include/pl_mpeg/pl_mpeg.h" \
  "$PREFIX/lib/libopenh264.a" \
  "$PREFIX/lib/libavcdec.a" \
  "$PREFIX/lib/libde265.a" \
  "$PREFIX/lib/libkvazaar.a" \
  "$PREFIX/lib/libfdk-aac.a" \
  "$PREFIX/lib/libmpeg2dec.a"; do
  need_file "$artifact"
done

cat > "$PREFIX/DVBSTREAMER5_NATIVE_CODECS.txt" <<INFO
DVBStreamer5 native codec prefix
OpenH264: 2.6.0 / 652bdb7719f30b52b08e506645a7322ff1b2cc6f
Ittiam libavc: 6d5853425d1697d6241a3e60ca6fd6c6c064cde6 (Apache-2.0)
libde265: 1.1.3 / ba62bf4cfb3242f3bf0a45617ff09e35236e4d82
Kvazaar: 2.3.2 / 6040962bed5cc68c5ad01234c38c08b8b2822068
FDK-AAC: 2.0.3 / 716f4394641d53f0d79c9ddac3fa93b03a49f278
Ittiam libmpeg2: e2dbb98d7819a8225687d3a0b7f3d818784e451c (Apache-2.0)
PL_MPEG: c871f2be022ece7ef4f64230b4fb8e1fb9eb6023
All codec libraries are built as project-local static inputs. No FFmpeg/libav/GStreamer.
INFO

echo "Native codec prefix ready: $PREFIX"
