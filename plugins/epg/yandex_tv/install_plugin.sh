#!/usr/bin/env bash
set -Eeuo pipefail
[[ $EUID -eq 0 ]] || { echo "Run with sudo" >&2; exit 1; }
BASE="$(cd -- "$(dirname -- "$0")" && pwd -P)"
[[ "$BASE" == "/opt/DVBStreamer5/scripts/epg-yandex-tv" ]] ||
  { echo "Install the portable DVBStreamer5 first; expected /opt/DVBStreamer5/scripts/epg-yandex-tv" >&2; exit 1; }
command -v python3 >/dev/null || { echo "Python 3 missing"; exit 1; }
CFG=/etc/dvbstreamer5/epg-yandex-tv.json
if [[ ! -e "$CFG" ]]; then
  install -D -m 0644 "$BASE/config.example.json" "$CFG"
  echo "Created disabled config: $CFG"
else
  echo "Preserved existing config: $CFG"
fi
install -m 0644 "$BASE/epg-yandex-tv.service" \
  /etc/systemd/system/epg-yandex-tv.service
systemctl daemon-reload
echo "Independent plugin installed, not enabled or started."
echo "Only if you have permission, edit $CFG to set permission_confirmed=true and enabled=true."
echo "Then: sudo systemctl enable --now epg-yandex-tv.service"
echo "Status: curl -fsS http://127.0.0.1:19785/health"
