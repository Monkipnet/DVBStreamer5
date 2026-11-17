#!/usr/bin/env bash
set -Eeuo pipefail

[[ $EUID -eq 0 ]] || { echo "Run with sudo" >&2; exit 1; }
BASE="$(cd -- "$(dirname -- "$0")" && pwd -P)"
[[ "$BASE" == "/opt/DVBStreamer5/scripts/epg-yandex-tv" ]] ||
  { echo "Install DVBStreamer5 portable first; expected /opt/DVBStreamer5/scripts/epg-yandex-tv" >&2; exit 1; }
command -v python3 >/dev/null || { echo "Python 3 missing" >&2; exit 1; }

# The main DVBStreamer5 configuration directory is 0700 root-owned.
# DynamicUser=yes cannot read a config within it. Keep this extension's
# public-feed settings separately; do not weaken DVBStreamer5 permissions.
CFG_DIR="/etc/epg-yandex-tv"
CFG="$CFG_DIR/config.json"
LEGACY="/etc/dvbstreamer5/epg-yandex-tv.json"
install -d -m 0755 "$CFG_DIR"

if [[ ! -e "$CFG" ]]; then
    if [[ -f "$LEGACY" ]]; then
        install -m 0644 -- "$LEGACY" "$CFG"
        echo "Migrated existing EPG plugin settings, legacy file preserved: $LEGACY"
    else
        install -m 0644 -- "$BASE/config.example.json" "$CFG"
        echo "Created disabled EPG plugin config: $CFG"
    fi
else
    echo "Preserving existing plugin config: $CFG"
fi

# Only public feed addresses and parser settings are supported here.
# Never store passwords, tokens or cookies in the 0644 config.
if [[ ! -r "$CFG" ]]; then
    echo "ERROR: cannot read config $CFG" >&2
    exit 1
fi
if [[ $(stat -c '%a' "$CFG") != 644 ]]; then
    echo "WARNING: plugin config mode differs from 0644: check DynamicUser access." >&2
fi

install -m 0644 "$BASE/epg-yandex-tv.service" \
  /etc/systemd/system/epg-yandex-tv.service
systemctl daemon-reload
echo "Independent EPG service installed but NOT automatically started."
echo "DVBStreamer5 and OSCam have not been restarted."
echo "Configuration: $CFG"
echo "Set enabled=true and permission_confirmed=true only with valid data reuse rights."
echo "Enable: sudo systemctl enable --now epg-yandex-tv.service"
echo "Restart plugin only: sudo systemctl restart epg-yandex-tv.service"
echo "Check: curl -v --connect-timeout 3 http://127.0.0.1:19785/health"
