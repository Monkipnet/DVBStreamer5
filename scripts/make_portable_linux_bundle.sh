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
shared-library dependencies. It also contains install.sh/uninstall.sh for a
systemd installation with runtime files in /opt/DVBStreamer5 and configuration
in /etc/dvbstreamer5. Hardware kernel/user drivers are intentionally not bundled.
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
export DVBSTREAMER5_CA_PLUGIN_DIR="\${DVBSTREAMER5_CA_PLUGIN_DIR:-\$ROOT/ca-plugins}"
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
[[ ! -s "\$ROOT/etc/ssl/certs/ca-certificates.crt" ]] || export SSL_CERT_FILE="\$ROOT/etc/ssl/certs/ca-certificates.crt"
[[ ! -d "\$ROOT/lib/ossl-modules" ]] || export OPENSSL_MODULES="\$ROOT/lib/ossl-modules"
exec "\$ROOT/lib/$LOADER_NAME" --library-path "\$ROOT/lib" "\$ROOT/oscam-mini/oscam-mini.bin" "\$@"
EOF_OSCAM
    chmod 0755 "$STAGE/oscam-mini/oscam-mini"
fi

cat > "$STAGE/install.sh" <<'EOF_INSTALL'
#!/usr/bin/env bash
set -Eeuo pipefail
umask 022

APP=DVBStreamer5
UNIT=dvbstreamer5.service
OSCAM_UNIT=oscam-mini.service
INSTALL_DIR=${DVBSTREAMER5_INSTALL_DIR:-/opt/DVBStreamer5}
CONFIG_DIR=${DVBSTREAMER5_CONFIG_DIR:-/etc/dvbstreamer5}
BUNDLE_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"

fail() { echo "ERROR: $*" >&2; exit 1; }
log() { printf '\n==> %s\n' "$*"; }

[[ $EUID -eq 0 ]] || fail "run as root: sudo ./install.sh"
[[ "$(uname -s)" == Linux ]] || fail "Linux is required"
[[ "$(uname -m)" == x86_64 ]] || fail "x86_64 is required"
command -v systemctl >/dev/null 2>&1 || fail "systemd/systemctl is required"
[[ -x "$BUNDLE_DIR/run.sh" && -x "$BUNDLE_DIR/bin/$APP" ]] || fail "bundle is incomplete"

log "Stopping existing services"
systemctl stop "$UNIT" 2>/dev/null || true
systemctl stop "$OSCAM_UNIT" 2>/dev/null || true

install -d -m0755 "$INSTALL_DIR"
install -d -m0700 "$CONFIG_DIR"
install -d -m0700 "$CONFIG_DIR/oscam-mini"
install -d -m0755 /var/lib/dvbstreamer5 /var/lib/dvbstreamer5/archive

# Migrate old in-tree configuration once. The encrypted UI password key must
# move together with dvbstreamer5-config.json.
for f in dvbstreamer5-config.json dvbstreamer5-subscribers.json dvbstreamer5-ui.key; do
    if [[ ! -e "$CONFIG_DIR/$f" && -e "$INSTALL_DIR/$f" ]]; then
        cp -a -- "$INSTALL_DIR/$f" "$CONFIG_DIR/$f"
        echo "Migrated $INSTALL_DIR/$f -> $CONFIG_DIR/$f"
    fi
done
if [[ -d "$INSTALL_DIR/oscam-mini/config" ]]; then
    for f in oscam.conf oscam.server oscam.user; do
        if [[ ! -e "$CONFIG_DIR/oscam-mini/$f" && -e "$INSTALL_DIR/oscam-mini/config/$f" ]]; then
            cp -a -- "$INSTALL_DIR/oscam-mini/config/$f" "$CONFIG_DIR/oscam-mini/$f"
            echo "Migrated OSCam config: $f"
        fi
    done
fi

log "Installing application runtime under $INSTALL_DIR"
for item in bin lib web ca-plugins etc run.sh PORTABILITY.txt BUNDLE-INFO.txt SHA256SUMS; do
    [[ -e "$BUNDLE_DIR/$item" ]] || continue
    rm -rf -- "$INSTALL_DIR/$item"
    cp -a -- "$BUNDLE_DIR/$item" "$INSTALL_DIR/$item"
done

if [[ -d "$BUNDLE_DIR/oscam-mini" && -x "$BUNDLE_DIR/oscam-mini/oscam-mini" ]]; then
    rm -rf -- "$INSTALL_DIR/oscam-mini"
    cp -a -- "$BUNDLE_DIR/oscam-mini" "$INSTALL_DIR/oscam-mini"
fi

chmod 0755 "$INSTALL_DIR/run.sh" "$INSTALL_DIR/bin/$APP"
if [[ -x "$INSTALL_DIR/oscam-mini/oscam-mini" ]]; then
    chmod 0755 "$INSTALL_DIR/oscam-mini/oscam-mini" "$INSTALL_DIR/oscam-mini/oscam-mini.bin"
fi
chmod 0700 "$CONFIG_DIR" "$CONFIG_DIR/oscam-mini"
find "$CONFIG_DIR" -maxdepth 2 -type f -exec chmod 0600 {} + 2>/dev/null || true

# Create default OSCam configuration only when no runtime config exists.
if [[ -d "$INSTALL_DIR/oscam-mini/default-config" ]]; then
    for f in oscam.conf oscam.server oscam.user; do
        if [[ ! -e "$CONFIG_DIR/oscam-mini/$f" && -e "$INSTALL_DIR/oscam-mini/default-config/$f" ]]; then
            install -m0600 "$INSTALL_DIR/oscam-mini/default-config/$f" "$CONFIG_DIR/oscam-mini/$f"
        fi
    done
fi

log "Installing systemd units"
cat > /etc/systemd/system/$UNIT <<EOF_MAIN
[Unit]
Description=DVBStreamer5 streaming service
Wants=network-online.target
After=network-online.target

[Service]
Type=simple
User=root
Group=root
WorkingDirectory=$INSTALL_DIR
Environment=DVBSTREAMER5_CONFIG_DIR=$CONFIG_DIR
Environment=DVBSTREAMER5_OSCAM_CONFIG_DIR=$CONFIG_DIR/oscam-mini
Environment=DVBSTREAMER5_OSCAM_BINARY=$INSTALL_DIR/oscam-mini/oscam-mini
Environment=DVBSTREAMER5_CA_PLUGIN_DIR=$INSTALL_DIR/ca-plugins
ExecStart=$INSTALL_DIR/run.sh
Restart=on-failure
RestartSec=3
TimeoutStopSec=35
LimitNOFILE=65536

[Install]
WantedBy=multi-user.target
EOF_MAIN

if [[ -x "$INSTALL_DIR/oscam-mini/oscam-mini" ]]; then
    cat > /etc/systemd/system/$OSCAM_UNIT <<EOF_OSCAM_UNIT
[Unit]
Description=DVBStreamer5 OSCam-mini Newcamd/PCSC/Phoenix card server
After=network.target
Conflicts=oscam.service
ConditionPathExists=$CONFIG_DIR/oscam-mini/oscam.conf

[Service]
Type=simple
User=root
Group=root
WorkingDirectory=$INSTALL_DIR/oscam-mini
ExecStart=$INSTALL_DIR/oscam-mini/oscam-mini -c $CONFIG_DIR/oscam-mini
SuccessExitStatus=15 SIGTERM
Restart=on-failure
RestartSec=2
Nice=5
LimitNOFILE=1024

[Install]
WantedBy=multi-user.target
EOF_OSCAM_UNIT
fi

systemctl daemon-reload

if systemctl list-unit-files oscam.service >/dev/null 2>&1; then
    systemctl disable --now oscam.service 2>/dev/null || true
fi
pkill -x oscam 2>/dev/null || true

if [[ -x "$INSTALL_DIR/oscam-mini/oscam-mini" ]]; then
    systemctl enable --now "$OSCAM_UNIT"
else
    echo "OSCam-mini is not present in this bundle; only DVBStreamer5 will be enabled."
fi
systemctl enable --now "$UNIT"

log "Installation complete"
echo "Program : $INSTALL_DIR"
echo "Config  : $CONFIG_DIR"
echo "Main    : systemctl status $UNIT --no-pager"
if [[ -x "$INSTALL_DIR/oscam-mini/oscam-mini" ]]; then
    echo "OSCam   : systemctl status $OSCAM_UNIT --no-pager"
fi
EOF_INSTALL
chmod 0755 "$STAGE/install.sh"

cat > "$STAGE/uninstall.sh" <<'EOF_UNINSTALL'
#!/usr/bin/env bash
set -Eeuo pipefail

INSTALL_DIR=${DVBSTREAMER5_INSTALL_DIR:-/opt/DVBStreamer5}
CONFIG_DIR=${DVBSTREAMER5_CONFIG_DIR:-/etc/dvbstreamer5}
PURGE=0
[[ "${1:-}" == "--purge-config" ]] && PURGE=1

[[ $EUID -eq 0 ]] || { echo "Run as root: sudo ./uninstall.sh" >&2; exit 1; }

systemctl disable --now dvbstreamer5.service 2>/dev/null || true
systemctl disable --now oscam-mini.service 2>/dev/null || true
rm -f /etc/systemd/system/dvbstreamer5.service /etc/systemd/system/oscam-mini.service
systemctl daemon-reload
rm -rf -- "$INSTALL_DIR"

if (( PURGE )); then
    rm -rf -- "$CONFIG_DIR"
    echo "Removed program and configuration."
else
    echo "Removed program. Configuration preserved in $CONFIG_DIR"
    echo "Use --purge-config to remove it too."
fi
EOF_UNINSTALL
chmod 0755 "$STAGE/uninstall.sh"

cat > "$STAGE/INSTALL.txt" <<'EOF_INSTALL_TXT'
DVBStreamer5 installable Linux bundle

Install:
  sudo ./install.sh

Installed layout:
  /opt/DVBStreamer5/               application, bundled loader and libraries
  /etc/dvbstreamer5/               DVBStreamer5 configuration
  /etc/dvbstreamer5/oscam-mini/    OSCam-mini configuration
  /var/lib/dvbstreamer5/            persistent runtime/archive data
  /etc/systemd/system/dvbstreamer5.service
  /etc/systemd/system/oscam-mini.service

The installer preserves existing /etc/dvbstreamer5 configuration and migrates
legacy configuration from /opt/DVBStreamer5 when the /etc copy does not exist.

Uninstall runtime but keep configuration:
  sudo ./uninstall.sh

Uninstall everything including configuration:
  sudo ./uninstall.sh --purge-config
EOF_INSTALL_TXT

cat > "$STAGE/PORTABILITY.txt" <<'EOF_PORTABILITY'
DVBStreamer5 portable Linux bundle

This directory can run in-place with ./run.sh or be installed with
sudo ./install.sh. The installed layout keeps all bundled user-space shared
libraries under /opt/DVBStreamer5/lib and stores mutable configuration under
/etc/dvbstreamer5.

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
echo "Run in-place on the target host:"
echo "  cd '$OUTPUT_DIR'"
echo "  ./run.sh"
echo
echo "Or install system-wide with systemd:"
echo "  sudo ./install.sh"
echo
echo "Bundle size: $(du -sh "$OUTPUT_DIR" | awk '{print $1}')"
echo "Shared libraries: ${#COPIED_NAMES[@]}"
echo "CA plugins: ${#PLUGINS[@]}"
[[ -z "$OSCAM_SOURCE" ]] || echo "OSCam-mini: included"
