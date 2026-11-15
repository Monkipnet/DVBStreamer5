#!/usr/bin/env bash
# DVBStreamer5 V546: entrypoint for two independent certificate modules.
# Arguments are validated twice: by C++ HTTP API and again here.
set -Eeuo pipefail
umask 077
(( EUID == 0 )) || { echo 'TLS operations require root' >&2; exit 2; }
HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
source "$HERE/modules/common.sh"
for cmd in flock openssl systemctl; do require_command "$cmd"; done
exec 9>/run/lock/dvbstreamer5-tls.lock
flock -n 9 || { echo "Another certificate operation is active" >&2; exit 3; }
# Capture the last live pair before any module changes links. A failed
# Nginx restart/configuration restores the previous certificate on disk too.
PREVIOUS_CERT="$(readlink "$TLS_DIR/active.crt" 2>/dev/null || true)"
PREVIOUS_KEY="$(readlink "$TLS_DIR/active.key" 2>/dev/null || true)"
restore_certificate_on_failure() {
  local code=$?
  if (( code != 0 )); then
    if [[ -n "$PREVIOUS_CERT" ]]; then
      ln -sfn "$PREVIOUS_CERT" "$TLS_DIR/active.crt"
    elif [[ -L "$TLS_DIR/active.crt" ]]; then
      rm -f "$TLS_DIR/active.crt"
    fi
    if [[ -n "$PREVIOUS_KEY" ]]; then
      ln -sfn "$PREVIOUS_KEY" "$TLS_DIR/active.key"
    elif [[ -L "$TLS_DIR/active.key" ]]; then
      rm -f "$TLS_DIR/active.key"
    fi
    if command -v nginx >/dev/null 2>&1 && nginx -t >/dev/null 2>&1; then
      systemctl reload nginx >/dev/null 2>&1 || true
    fi
  fi
}
trap restore_certificate_on_failure EXIT
ACTION="${1:-}"
MODE=none AUTO_RENEW=false ISSUER=none
case "$ACTION" in
  selfsigned)
    (( $# == 4 )) || die_tls invalid_arguments
    DOMAIN="$2"; BACKEND_PORT="$3"; HTTPS_PORT="$4"
    check_domain "$DOMAIN"; check_port "$BACKEND_PORT"; check_port "$HTTPS_PORT"
    [[ "$BACKEND_PORT" != "$HTTPS_PORT" && "$HTTPS_PORT" != 80 ]] || die_tls invalid_ports
    source "$HERE/modules/selfsigned.sh"
    ;;
  le-enable)
    (( $# == 5 )) || die_tls invalid_arguments
    DOMAIN="$2"; EMAIL="$3"; BACKEND_PORT="$4"; HTTPS_PORT="$5"
    source "$HERE/modules/letsencrypt.sh"
    le_enable
    ;;
  le-disable)
    (( $# == 1 )) || die_tls invalid_arguments
    source "$HERE/modules/letsencrypt.sh"
    le_disable
    ;;
  le-renew)
    (( $# == 1 )) || die_tls invalid_arguments
    source "$HERE/modules/letsencrypt.sh"
    le_renew
    ;;
  *) die_tls invalid_action;;
esac
