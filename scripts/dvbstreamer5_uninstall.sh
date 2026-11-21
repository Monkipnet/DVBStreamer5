#!/usr/bin/env bash
# Uninstall only the installed DVBStreamer5 copy containing this script.
set -Eeuo pipefail
umask 077

fail() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }
[[ ${EUID} -eq 0 ]] || fail 'Run as root (sudo /opt/DVBStreamer5-XX/uninstall.sh)'
[[ $# -eq 0 || ( $# -eq 1 && "$1" == '--purge-config' ) ]] || fail 'Usage: uninstall.sh [--purge-config]'
PURGE=0
[[ ${1:-} != '--purge-config' ]] || PURGE=1
SCRIPT_PATH="$(readlink -f -- "${BASH_SOURCE[0]}")"
INSTALL_DIR="$(dirname -- "$SCRIPT_PATH")"
[[ -f "$INSTALL_DIR/VERSION" && -x "$INSTALL_DIR/run.sh" ]] || fail 'Not an installed DVBStreamer5 runtime directory'
[[ ! -L "$INSTALL_DIR" ]] || fail 'Symlinked installation directories are not supported'
case "$INSTALL_DIR" in
    /opt/DVBStreamer5)
        INSTANCE='legacy'
        UNIT='dvbstreamer5.service'
        CONFIG_DIR='/etc/dvbstreamer5'
        OSCAM_UNIT='oscam-mini.service'
        ;;
    /opt/DVBStreamer5-*)
        INSTANCE="${INSTALL_DIR#/opt/DVBStreamer5-}"
        [[ "$INSTANCE" =~ ^[a-zA-Z0-9][a-zA-Z0-9_-]{0,31}$ ]] || fail 'Unsafe instance name'
        UNIT="dvbstreamer5@${INSTANCE}.service"
        CONFIG_DIR="/etc/dvbstreamer5/instances/$INSTANCE"
        OSCAM_UNIT="oscam-mini@${INSTANCE}.service"
        ;;
    *) fail "Unrecognized runtime location: $INSTALL_DIR" ;;
esac

# Never trust environment variables to pick an installation to remove.
# A unit must explicitly refer back to this specific runtime.
unit_file="/etc/systemd/system/$UNIT"
[[ -f "$unit_file" && ! -L "$unit_file" ]] || fail "Expected unit not found: $UNIT"
grep -Fxq "WorkingDirectory=$INSTALL_DIR" "$unit_file" || fail "Unit does not own this runtime: $UNIT"
grep -Fxq "ExecStart=$INSTALL_DIR/run.sh" "$unit_file" || fail "Unit executable does not match this runtime: $UNIT"

if [[ "$INSTANCE" == legacy ]]; then
    [[ -z "$(find /etc/systemd/system -maxdepth 1 -name 'dvbstreamer5@*.service' -print -quit)" ]] ||
        printf 'Other DVBStreamer5 instances detected; they will NOT be touched.\n'
fi
printf 'Target: %s\nService: %s\nConfig: %s\n' "$INSTALL_DIR" "$UNIT" "$CONFIG_DIR"
if (( PURGE )); then
    printf 'Configuration purge requested for THIS instance only.\n'
fi

systemctl stop "$UNIT"
systemctl disable "$UNIT"
# OSCam-mini is separately scoped; only stop it if its installed unit belongs
# to this runtime, never disable a shared OSCam service.
oscam_path="/etc/systemd/system/$OSCAM_UNIT"
if [[ -f "$oscam_path" && ! -L "$oscam_path" ]] &&
   grep -Fq "$INSTALL_DIR/oscam-mini/oscam-mini" "$oscam_path"; then
    systemctl stop "$OSCAM_UNIT"
    systemctl disable "$OSCAM_UNIT"
    rm -f -- "$oscam_path"
fi
rm -f -- "$unit_file"
systemctl daemon-reload
rm -rf --one-file-system -- "$INSTALL_DIR"
if (( PURGE )); then
    if [[ "$INSTANCE" == legacy ]]; then
        # This directory may also contain instances/ used by other services.
        # Never recursively remove it or touch its child instance directories.
        for file in dvbstreamer5-config.json dvbstreamer5-subscribers.json dvbstreamer5-ui.key; do
            rm -f -- "$CONFIG_DIR/$file"
        done
        # Legacy OSCam config is intentionally retained unless independently
        # proven to be private; it may be shared by another installed copy.
    else
        [[ "$CONFIG_DIR" == "/etc/dvbstreamer5/instances/$INSTANCE" ]] ||
            fail 'Configuration path rejected'
        rm -rf --one-file-system -- "$CONFIG_DIR"
        STATE_DIR="/var/lib/dvbstreamer5/instances/$INSTANCE"
        if [[ -d "$STATE_DIR" && ! -L "$STATE_DIR" ]]; then
            rm -rf --one-file-system -- "$STATE_DIR"
        fi
    fi
    printf 'Removed this runtime and its configuration.\n'
else
    printf 'Removed this runtime. Configuration preserved: %s\n' "$CONFIG_DIR"
fi
