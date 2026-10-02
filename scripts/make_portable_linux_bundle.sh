#!/usr/bin/env bash
set -Eeuo pipefail
umask 022

APP="DVBStreamer5"
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
PROJECT_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd -P)"
BUILD_DIR="$PROJECT_ROOT/build"
OUTPUT_DIR=""
BINARY=""
DO_BUILD=0
DO_ARCHIVE=0
INCLUDE_OSCAM=auto
JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || nproc 2>/dev/null || echo 2)"

usage() {
    cat <<'USAGE'
Create a self-contained DVBStreamer5 Linux x86_64 runtime bundle.

Usage:
  bash scripts/make_portable_linux_bundle.sh [options]

Options:
  --build-dir DIR       CMake build directory (default: ./build)
  --binary FILE         Exact DVBStreamer5 executable to package
  --output-dir DIR      Bundle directory (default: dist/DVBStreamer5-linux-x86_64)
  --build               Build DVBStreamer5 before packaging
  --jobs N              Parallel build jobs used with --build
  --with-oscam          Require and include build/oscam-mini/oscam-mini
  --without-oscam       Do not include oscam-mini
  --archive             Also create <bundle>.tar.gz
  -h, --help            Show this help

The bundle contains the application, web assets, CA plugins found in the build
folder, optional oscam-mini, the ELF interpreter and recursively discovered
shared-library dependencies. Hardware kernel/user drivers are intentionally not
bundled; the target host still needs the appropriate GPU/DVB/smart-card driver.
USAGE
}

fail() { echo "ERROR: $*" >&2; exit 1; }
log() { printf '\n==> %s\n' "$*"; }

while (($#)); do
    case "$1" in
        --build-dir|--binary|--output-dir|--jobs)
            (($# >= 2)) || fail "$1 requires a value"
            case "$1" in
                --build-dir) BUILD_DIR="$2" ;;
                --binary) BINARY="$2" ;;
                --output-dir) OUTPUT_DIR="$2" ;;
                --jobs) JOBS="$2" ;;
            esac
            shift 2 ;;
        --build) DO_BUILD=1; shift ;;
        --archive) DO_ARCHIVE=1; shift ;;
        --with-oscam) INCLUDE_OSCAM=yes; shift ;;
        --without-oscam) INCLUDE_OSCAM=no; shift ;;
        -h|--help) usage; exit 0 ;;
        *) fail "unknown option: $1" ;;
    esac
done

[[ "$(uname -s)" == Linux ]] || fail "this packager must run on Linux"
[[ "$(uname -m)" == x86_64 ]] || fail "only x86_64 bundles are supported; current: $(uname -m)"
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || fail "--jobs must be a positive integer"

for tool in ldd readelf file awk sed find sha256sum; do
    command -v "$tool" >/dev/null 2>&1 || fail "required tool is missing: $tool"
done

[[ "$BUILD_DIR" == /* ]] || BUILD_DIR="$PROJECT_ROOT/$BUILD_DIR"
OUTPUT_DIR="${OUTPUT_DIR:-$PROJECT_ROOT/dist/${APP}-linux-x86_64}"
[[ "$OUTPUT_DIR" == /* ]] || OUTPUT_DIR="$PROJECT_ROOT/$OUTPUT_DIR"

if (( DO_BUILD )); then
    [[ -d "$BUILD_DIR" ]] || fail "build directory does not exist: $BUILD_DIR (configure it with cmake -S . -B ...)"
    log "Building $APP"
    cmake --build "$BUILD_DIR" --parallel "$JOBS" --target "$APP"
fi

if [[ -z "$BINARY" ]]; then
    for candidate in "$BUILD_DIR/$APP" "$PROJECT_ROOT/$APP"; do
        if [[ -f "$candidate" && -x "$candidate" ]]; then
            BINARY="$candidate"
            break
        fi
    done
fi
[[ -n "$BINARY" ]] || fail "DVBStreamer5 executable not found; pass --binary or --build-dir"
[[ "$BINARY" == /* ]] || BINARY="$PROJECT_ROOT/$BINARY"
[[ -f "$BINARY" && -x "$BINARY" ]] || fail "not an executable file: $BINARY"
file -b "$BINARY" | grep -q 'ELF 64-bit' || fail "not a 64-bit ELF executable: $BINARY"

WEB_DIR="$PROJECT_ROOT/web"
[[ -d "$WEB_DIR" ]] || fail "web directory is missing: $WEB_DIR"

INTERPRETER="$(LC_ALL=C readelf -l "$BINARY" | sed -n 's@.*Requesting program interpreter: \([^]]*\).*@\1@p' | head -n1)"
[[ -n "$INTERPRETER" && -f "$INTERPRETER" ]] || fail "cannot resolve ELF interpreter for $BINARY"
LOADER_NAME="$(basename -- "$INTERPRETER")"

TMP="$(mktemp -d "${TMPDIR:-/tmp}/dvbstreamer5-bundle.XXXXXXXX")"
cleanup() { rm -rf -- "$TMP"; }
trap cleanup EXIT

STAGE="$TMP/bundle"
mkdir -p "$STAGE/bin" "$STAGE/lib" "$STAGE/web" "$STAGE/ca-plugins" \
         "$STAGE/oscam-mini" "$STAGE/etc/ssl/certs" "$STAGE/data"

install -m 0755 "$BINARY" "$STAGE/bin/$APP"
cp -a -- "$WEB_DIR/." "$STAGE/web/"

mapfile -t PLUGINS < <(find "$BUILD_DIR" -type f -name 'dvbstreamer5-ca-*.so' -print 2>/dev/null | sort -u)
for plugin in "${PLUGINS[@]}"; do
    install -m 0644 "$plugin" "$STAGE/ca-plugins/$(basename -- "$plugin")"
done

OSCAM_SOURCE=""
if [[ "$INCLUDE_OSCAM" != no ]]; then
    for candidate in "$BUILD_DIR/oscam-mini/oscam-mini" "$PROJECT_ROOT/oscam-mini/oscam-mini"; do
        if [[ -f "$candidate" && -x "$candidate" ]]; then
            OSCAM_SOURCE="$candidate"
            break
        fi
    done
    [[ "$INCLUDE_OSCAM" != yes || -n "$OSCAM_SOURCE" ]] || fail "--with-oscam was requested but oscam-mini was not found"
fi
if [[ -n "$OSCAM_SOURCE" ]]; then
    install -m 0755 "$OSCAM_SOURCE" "$STAGE/oscam-mini/oscam-mini.bin"
    if [[ -d "$PROJECT_ROOT/packaging/oscam-mini/default-config" ]]; then
        cp -a -- "$PROJECT_ROOT/packaging/oscam-mini/default-config" "$STAGE/oscam-mini/default-config"
    fi
    mkdir -p "$STAGE/oscam-mini/config"
fi

for cert in /etc/ssl/certs/ca-certificates.crt /etc/pki/tls/certs/ca-bundle.crt /etc/ssl/ca-bundle.pem; do
    if [[ -s "$cert" ]]; then
        cp -L -- "$cert" "$STAGE/etc/ssl/certs/ca-certificates.crt"
        break
    fi
done

declare -A SEEN_OBJECTS=()
declare -A COPIED_NAMES=()
QUEUE=("$BINARY")
for plugin in "${PLUGINS[@]}"; do QUEUE+=("$plugin"); done
[[ -z "$OSCAM_SOURCE" ]] || QUEUE+=("$OSCAM_SOURCE")

list_deps() {
    local object="$1" output missing
    output="$(LC_ALL=C ldd "$object" 2>&1)" || {
        echo "$output" >&2
        fail "ldd failed for $object"
    }
    missing="$(printf '%s\n' "$output" | awk '/=> not found/ {print}')"
    [[ -z "$missing" ]] || fail "missing shared libraries for $object: $missing"
    printf '%s\n' "$output" | awk '
        /=> \/[^ ]+/ { print $3; next }
        /^[[:space:]]*\/[^ ]+/ { print $1; next }
    ' | sort -u
}

copy_library() {
    local src="$1" base dest old_hash new_hash
    [[ -f "$src" ]] || fail "dependency path does not exist: $src"
    base="$(basename -- "$src")"
    dest="$STAGE/lib/$base"
    if [[ -e "$dest" ]]; then
        old_hash="$(sha256sum "$dest" | awk '{print $1}')"
        new_hash="$(sha256sum "$src" | awk '{print $1}')"
        [[ "$old_hash" == "$new_hash" ]] || fail "library basename collision: $base ($src conflicts with bundled copy)"
        return 0
    fi
    cp -L --preserve=mode,timestamps -- "$src" "$dest"
    COPIED_NAMES["$base"]="$src"
    QUEUE+=("$dest")
}

copy_library "$INTERPRETER"

drain_dependency_queue() {
    local object real dep
    while (("${#QUEUE[@]}")); do
        object="${QUEUE[0]}"
        QUEUE=("${QUEUE[@]:1}")
        real="$(readlink -f -- "$object")"
        [[ -z "${SEEN_OBJECTS[$real]:-}" ]] || continue
        SEEN_OBJECTS["$real"]=1
        while IFS= read -r dep; do
            [[ -n "$dep" ]] || continue
            copy_library "$dep"
        done < <(list_deps "$object")
    done
}

drain_dependency_queue

LIBC_SOURCE="${COPIED_NAMES[libc.so.6]:-}"
if [[ -n "$LIBC_SOURCE" ]]; then
    LIBC_DIR="$(dirname -- "$(readlink -f -- "$LIBC_SOURCE")")"
    for nss in libnss_files.so.2 libnss_dns.so.2 libnss_compat.so.2 libresolv.so.2; do
        [[ -f "$LIBC_DIR/$nss" ]] && copy_library "$LIBC_DIR/$nss"
    done
    drain_dependency_queue
fi

OPENSSL_MODULE_DIR=""
if command -v openssl >/dev/null 2>&1; then
    OPENSSL_MODULE_DIR="$(openssl version -m 2>/dev/null | sed -n 's/^MODULESDIR: "\(.*\)"$/\1/p')"
fi
if [[ -n "$OPENSSL_MODULE_DIR" && -d "$OPENSSL_MODULE_DIR" ]]; then
    mkdir -p "$STAGE/lib/ossl-modules"
    while IFS= read -r module; do
        dest="$STAGE/lib/ossl-modules/$(basename -- "$module")"
        cp -L --preserve=mode,timestamps -- "$module" "$dest"
        QUEUE+=("$dest")
    done < <(find "$OPENSSL_MODULE_DIR" -maxdepth 1 -type f -name '*.so' -print | sort)
    drain_dependency_queue
fi

cat > "$STAGE/run.sh" <<EOF_RUN
#!/usr/bin/env bash
set -Eeuo pipefail
ROOT="\$(cd -- "\$(dirname -- "\${BASH_SOURCE[0]}")" && pwd -P)"
export LD_LIBRARY_PATH="\$ROOT/lib\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}"
[[ ! -s "\$ROOT/etc/ssl/certs/ca-certificates.crt" ]] || export SSL_CERT_FILE="\$ROOT/etc/ssl/certs/ca-certificates.crt"
[[ ! -d "\$ROOT/lib/ossl-modules" ]] || export OPENSSL_MODULES="\$ROOT/lib/ossl-modules"
cd "\$ROOT"
exec "\$ROOT/lib/$LOADER_NAME" --library-path "\$ROOT/lib" "\$ROOT/bin/$APP" "\$@"
EOF_RUN
chmod 0755 "$STAGE/run.sh"

if [[ -n "$OSCAM_SOURCE" ]]; then
    cat > "$STAGE/oscam-mini/oscam-mini" <<EOF_OSCAM
#!/usr/bin/env bash
set -Eeuo pipefail
ROOT="\$(cd -- "\$(dirname -- "\${BASH_SOURCE[0]}")/.." && pwd -P)"
export LD_LIBRARY_PATH="\$ROOT/lib\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}"
[[ ! -d "\$ROOT/lib/ossl-modules" ]] || export OPENSSL_MODULES="\$ROOT/lib/ossl-modules"
exec "\$ROOT/lib/$LOADER_NAME" --library-path "\$ROOT/lib" "\$ROOT/oscam-mini/oscam-mini.bin" "\$@"
EOF_OSCAM
    chmod 0755 "$STAGE/oscam-mini/oscam-mini"
fi

cat > "$STAGE/PORTABILITY.txt" <<'EOF_PORTABILITY'
DVBStreamer5 portable Linux bundle

This directory is intended to run without installing the user-space shared
libraries that were present on the build machine. Start it with ./run.sh.

Compatibility boundary:
- CPU architecture must be x86_64.
- The target Linux kernel must be new enough for the glibc copied from the build host.
- GPU/DVB/USB/smart-card kernel drivers are host-specific and are NOT bundled.
- VAAPI/QSV/NVIDIA hardware acceleration still requires a compatible host driver
  and /dev/dri (or NVIDIA device nodes). NVIDIA driver libraries may be loaded
  dynamically by the driver stack and intentionally remain host-provided.
- DVB tuners require the target kernel's DVB drivers/firmware and /dev/dvb.
- PC/SC readers require USB permissions and a running pcscd when oscam-mini uses PC/SC.

For the widest compatibility, create the bundle on the oldest Linux distribution
that you intend to support, then run the resulting folder on equal/newer systems.
EOF_PORTABILITY

{
    echo "app=$APP"
    echo "build_host=$(uname -srmo)"
    echo "binary_source=$BINARY"
    echo "elf_interpreter=$INTERPRETER"
    echo "created_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "libraries=${#COPIED_NAMES[@]}"
    echo "plugins=${#PLUGINS[@]}"
    [[ -z "$OSCAM_SOURCE" ]] || echo "oscam_source=$OSCAM_SOURCE"
} > "$STAGE/BUNDLE-INFO.txt"

(
    cd "$STAGE"
    find . -type f ! -name SHA256SUMS -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS
)

log "Verifying bundled runtime"
"$STAGE/lib/$LOADER_NAME" --library-path "$STAGE/lib" --verify "$STAGE/bin/$APP" \
    || fail "bundled ELF loader cannot verify DVBStreamer5"

rm -rf -- "$OUTPUT_DIR"
mkdir -p -- "$(dirname -- "$OUTPUT_DIR")"
mv -- "$STAGE" "$OUTPUT_DIR"
trap - EXIT
rm -rf -- "$TMP"

if (( DO_ARCHIVE )); then
    ARCHIVE="${OUTPUT_DIR%/}.tar.gz"
    rm -f -- "$ARCHIVE"
    tar -C "$(dirname -- "$OUTPUT_DIR")" -czf "$ARCHIVE" "$(basename -- "$OUTPUT_DIR")"
    log "Archive: $ARCHIVE"
fi

log "Portable bundle created: $OUTPUT_DIR"
echo "Run on the target host:"
echo "  cd '$OUTPUT_DIR'"
echo "  ./run.sh"
echo
echo "Bundle size: $(du -sh "$OUTPUT_DIR" | awk '{print $1}')"
echo "Shared libraries: ${#COPIED_NAMES[@]}"
echo "CA plugins: ${#PLUGINS[@]}"
[[ -z "$OSCAM_SOURCE" ]] || echo "OSCam-mini: included"
