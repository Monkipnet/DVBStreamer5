#!/usr/bin/env bash
# Portable multi-instance dispatcher. Run from the extracted, verified bundle.
set -Eeuo pipefail
umask 077
BUNDLE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
fail(){ printf 'ERROR: %s\n' "$*" >&2; exit 1; }
log(){ printf '%s\n' "$*"; }
[[ $EUID == 0 ]] || fail 'Run with sudo/root'
[[ -x "$BUNDLE/run.sh" && -f "$BUNDLE/VERSION" && -x "$BUNDLE/legacy-install.sh" ]] || fail 'Incomplete portable installer'
command -v systemctl >/dev/null || fail 'systemd is required'
ACTION="${1:-}"
NAME=''
PORT=''
while (($#)); do
  case "$1" in
    --update-all|--new|--update-instance) [[ -z "$ACTION" || "$ACTION" == "$1" ]] || fail 'Conflicting actions'; ACTION="$1"; shift ;;
    --name) (($#>1)) || fail 'Missing --name'; NAME="$2"; shift 2 ;;
    --port) (($#>1)) || fail 'Missing --port'; PORT="$2"; shift 2 ;;
    *) fail "Unknown argument: $1" ;;
  esac
done
# No args: require an explicit choice if any runtime is already installed.
if [[ -z "$ACTION" ]]; then
  if [[ ! -x /opt/DVBStreamer5/run.sh ]] &&
     ! compgen -G '/opt/DVBStreamer5-*' >/dev/null; then
    ACTION='--first'
  elif [[ -t 0 ]]; then
    printf '1) Update ALL installed DVBStreamer5 copies\n2) Install ADDITIONAL independent copy\nSelect [1/2]: '
    read -r answer
    case "$answer" in 1) ACTION='--update-all';; 2) ACTION='--new';; *) fail 'Invalid selection';; esac
  else
    # Compatibility with V10.8.618 and older web updaters: these run
    # bundle/install.sh without arguments in a detached systemd worker.
    # Updating every existing copy is the documented installer default.
    log 'Noninteractive legacy updater detected: updating installed copies.'
    ACTION='--update-all'
  fi
fi
validate_name(){
  [[ "$NAME" =~ ^[a-zA-Z0-9][a-zA-Z0-9_-]{0,31}$ ]] || fail 'Invalid instance name'
}
validate_port(){
  [[ "$PORT" =~ ^[0-9]{2,5}$ ]] && ((10#$PORT>=1024 && 10#$PORT<=65535)) || fail 'Invalid port (1024..65535)'
  PORT="$((10#$PORT))"
}
install_named(){
  local dir="/opt/DVBStreamer5-$NAME" conf="/etc/dvbstreamer5/instances/$NAME"
  local state="/var/lib/dvbstreamer5/instances/$NAME" unit="dvbstreamer5@$NAME.service"
  local fresh="$1" item
  if [[ "$fresh" == yes ]]; then
    [[ ! -e "$dir" && ! -e "/etc/systemd/system/$unit" ]] || fail 'Instance already exists'
    if command -v ss >/dev/null; then
      if ss -H -ltnu | awk '{print $5}' | grep -Eq "[:.]$PORT$"; then fail "Port $PORT is already in use"; fi
    else fail 'ss is required to verify port availability'; fi
    # Prevent duplicate management ports recorded in other instances, including stopped copies.
    if command -v python3 >/dev/null; then
      python3 - "$PORT" <<'PY'
import glob,json,sys
port=int(sys.argv[1])
files=glob.glob('/etc/dvbstreamer5/instances/*/dvbstreamer5-config.json')
files+=['/etc/dvbstreamer5/dvbstreamer5-config.json']
for path in files:
  try:
    with open(path) as f: configured=int(json.load(f).get('http_port',9000))
  except (OSError,ValueError,TypeError): continue
  if configured==port: raise SystemExit('Port reserved in '+path)
PY
    else fail 'python3 required for port collision check'; fi
  else
    [[ -d "$dir" && -f "/etc/systemd/system/$unit" ]] || fail 'Existing instance not found'
    grep -Fxq "WorkingDirectory=$dir" "/etc/systemd/system/$unit" || fail 'Instance ownership check failed'
  fi
  mkdir -p -m 0755 "$dir" "$state" "$state/epg" "$state/archive"
  mkdir -p -m 0700 "$conf"
  if [[ "$fresh" == yes ]]; then
    printf '{"http_port":%s,"server_name":"DVBStreamer5 %s","streams":[]}\n' "$PORT" "$NAME" > "$conf/dvbstreamer5-config.json"
    chmod 0600 "$conf/dvbstreamer5-config.json"
  fi
  local backup=''
  if [[ "$fresh" != yes ]]; then
    backup="$(mktemp -d -p /var/tmp "dvbstreamer5-$NAME-rollback.XXXXXXXX")"
    cp -a -- "$dir" "$backup/runtime"
    cp -a -- "/etc/systemd/system/$unit" "$backup/service"
    systemctl stop "$unit"
  fi
  for item in bin lib web ca-plugins etc tls run.sh updater.sh uninstall.sh VERSION PORTABILITY.txt BUNDLE-INFO.txt SHA256SUMS scripts oscam-mini; do
    [[ -e "$BUNDLE/$item" ]] || continue
    rm -rf -- "$dir/$item"
    cp -a -- "$BUNDLE/$item" "$dir/$item"
  done
  cat > "/etc/systemd/system/$unit" <<EOF_UNIT
[Unit]
Description=DVBStreamer5 independent instance $NAME
Wants=network-online.target
After=network-online.target

[Service]
Type=simple
User=root
Group=root
WorkingDirectory=$dir
Environment=DVBSTREAMER5_CONFIG_DIR=$conf
Environment=DVBSTREAMER5_EPG_CACHE_DIR=$state/epg
Environment=DVBSTREAMER5_INSTANCE_ID=$NAME
Environment=DVBSTREAMER5_STATE_DIR=$state
Environment=DVBSTREAMER5_CA_PLUGIN_DIR=$dir/ca-plugins
Environment=DVBSTREAMER5_OSCAM_CONFIG_DIR=$conf/oscam-mini
Environment=DVBSTREAMER5_OSCAM_BINARY=$dir/oscam-mini/oscam-mini
ExecStart=$dir/run.sh
Restart=on-failure
RestartSec=3
TimeoutStopSec=20
KillMode=control-group
LimitNOFILE=65536

[Install]
WantedBy=multi-user.target
EOF_UNIT
  systemctl daemon-reload
  if ! systemctl enable --now "$unit" || ! systemctl is-active --quiet "$unit"; then
    if [[ -n "$backup" ]]; then
      systemctl stop "$unit" || true
      rm -rf -- "$dir"
      mv -- "$backup/runtime" "$dir"
      cp -a -- "$backup/service" "/etc/systemd/system/$unit"
      systemctl daemon-reload
      systemctl start "$unit" || true
    else
      systemctl disable --now "$unit" || true
      rm -f -- "/etc/systemd/system/$unit"
      systemctl daemon-reload
      rm -rf -- "$dir"
    fi
    fail "Instance $NAME failed to start; previous runtime restored when available"
  fi
  [[ -z "$backup" ]] || rm -rf -- "$backup"
  log "Installed $NAME at $dir; service $unit"
}
case "$ACTION" in
  --first)
    exec /bin/bash "$BUNDLE/legacy-install.sh" ;;
  --new)
    if [[ -z "$NAME" ]]; then
      [[ -t 0 ]] || fail 'Provide --name'
      read -r -p 'New instance ID (e.g. 02): ' NAME
    fi
    validate_name
    if [[ -z "$PORT" ]]; then
      [[ -t 0 ]] || fail 'Provide --port'
      read -r -p 'New HTTP port: ' PORT
    fi
    validate_port
    install_named yes ;;
  --update-instance)
    validate_name
    [[ -z "$PORT" ]] || fail 'Port cannot change during update'
    install_named no ;;
  --update-all)
    found=0
    # Legacy installation first; preserve the legacy updater and configuration.
    if [[ -x /opt/DVBStreamer5/run.sh ]]; then
      found=1
      /bin/bash "$BUNDLE/legacy-install.sh"
    fi
    for dir in /opt/DVBStreamer5-*; do
      [[ -d "$dir" && -x "$dir/run.sh" ]] || continue
      NAME="${dir#/opt/DVBStreamer5-}"
      validate_name
      found=1
      install_named no
    done
    ((found)) || fail 'No installed instances found' ;;
  *) fail "Unsupported mode: $ACTION" ;;
esac
