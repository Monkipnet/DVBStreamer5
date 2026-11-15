#!/usr/bin/env bash
# V546 independent opt-in Let's Encrypt issuance / renewal module.
set -Eeuo pipefail
le_bootstrap() {
  require_command openssl
  if [[ -s "$TLS_DIR/active.crt" && -s "$TLS_DIR/active.key" ]] &&
     openssl x509 -in "$TLS_DIR/active.crt" -noout -checkend 86400 >/dev/null 2>&1; then
    return 0
  fi
  local work folder
  work="$(mktemp -d "$TLS_DIR/.bootstrap.XXXXXXXX")"
  openssl req -x509 -newkey rsa:3072 -sha256 -nodes -days 365 \
    -keyout "$work/privkey.pem" -out "$work/fullchain.pem" \
    -subj "/CN=$DOMAIN" -addext "subjectAltName=DNS:$DOMAIN" >/dev/null 2>&1 ||
    die_tls openssl_fallback_failed
  folder="$TLS_DIR/selfsigned/$(date +%s)-$$"
  mkdir -p "$TLS_DIR/selfsigned"
  chmod 0700 "$TLS_DIR/selfsigned"
  mv "$work" "$folder"
  chmod 0600 "$folder/privkey.pem"
  activate_certificate "$folder/fullchain.pem" "$folder/privkey.pem"
}
le_timer() {
  cat > /etc/systemd/system/dvbstreamer5-le-renew.service <<UNIT
[Unit]
Description=DVBStreamer5 Let's Encrypt renewal check (opt-in)
After=network-online.target
Wants=network-online.target
[Service]
Type=oneshot
ExecStart=/bin/bash $HERE/manage.sh le-renew
TimeoutStartSec=600
UNIT
  cat > /etc/systemd/system/dvbstreamer5-le-renew.timer <<'UNIT'
[Unit]
Description=DVBStreamer5 opt-in daily Let's Encrypt check
[Timer]
OnCalendar=daily
RandomizedDelaySec=1h
Persistent=true
[Install]
WantedBy=timers.target
UNIT
  systemctl daemon-reload
  systemctl enable --now dvbstreamer5-le-renew.timer >/dev/null ||
    die_tls timer_enable_failed
}
le_enable() {
  MODE=letsencrypt AUTO_RENEW=false ISSUER=letsencrypt
  : "${DOMAIN:?}" "${EMAIL:?}" "${BACKEND_PORT:?}" "${HTTPS_PORT:?}"
  check_domain "$DOMAIN"; check_email "$EMAIL"
  check_port "$BACKEND_PORT"; check_port "$HTTPS_PORT"
  [[ "$BACKEND_PORT" != "$HTTPS_PORT" && "$HTTPS_PORT" != 80 ]] || die_tls invalid_ports
  nginx_preflight "$HTTPS_PORT"
  require_command openssl
  if ! command -v certbot >/dev/null 2>&1; then
    status running letsencrypt installing_certbot true none
    if command -v apt-get >/dev/null 2>&1; then
      DEBIAN_FRONTEND=noninteractive apt-get update -qq &&
      DEBIAN_FRONTEND=noninteractive apt-get install -y -qq certbot ||
        die_tls certbot_install_failed
    else
      die_tls certbot_unavailable
    fi
  fi
  # Renewals use this root-only file. No user-controlled paths or commands.
  printf 'DOMAIN=%q\nEMAIL=%q\nBACKEND_PORT=%q\nHTTPS_PORT=%q\n' \
    "$DOMAIN" "$EMAIL" "$BACKEND_PORT" "$HTTPS_PORT" >"$SETTINGS.tmp.$$"
  chmod 0600 "$SETTINGS.tmp.$$"
  mv -f "$SETTINGS.tmp.$$" "$SETTINGS"
  status running letsencrypt requesting true none
  le_bootstrap
  # Create ACME-only HTTP-01 route. Certificate requests must not be proxied.
  nginx_managed_proxy "$DOMAIN" "$BACKEND_PORT" "$HTTPS_PORT" true
  le_timer
  AUTO_RENEW=true
  if certbot certonly --config-dir "$ACME_CONFIG" --work-dir "$ACME_WORK" \
       --logs-dir "$ACME_LOG" --webroot -w "$ACME_ROOT" -d "$DOMAIN" \
       --non-interactive --agree-tos --email "$EMAIL" \
       --keep-until-expiring --preferred-challenges http >/dev/null 2>&1; then
    local cert="$ACME_CONFIG/live/$DOMAIN/fullchain.pem"
    local key="$ACME_CONFIG/live/$DOMAIN/privkey.pem"
    activate_certificate "$cert" "$key"
    nginx -t >/dev/null 2>&1 || die_tls nginx_config_invalid
    systemctl reload nginx >/dev/null 2>&1 || die_tls nginx_reload_failed
    status ok letsencrypt letsencrypt_certificate_active true letsencrypt
  else
    # Keep working TLS and daily retry; never replace valid LE by a new
    # self-signed certificate because of a transient ACME failure.
    status warning letsencrypt acme_failed_retry_scheduled true fallback
  fi
}
le_disable() {
  MODE=letsencrypt AUTO_RENEW=false ISSUER=none
  if [[ -f "$SETTINGS" ]]; then
    # shellcheck disable=SC1090
    source "$SETTINGS"
  fi
  systemctl disable --now dvbstreamer5-le-renew.timer >/dev/null 2>&1 || true
  rm -f "$SETTINGS"
  if [[ -n "${DOMAIN:-}" && -n "${BACKEND_PORT:-}" && -n "${HTTPS_PORT:-}" &&
        -e "$TLS_DIR/nginx-managed" ]]; then
    nginx_managed_proxy "$DOMAIN" "$BACKEND_PORT" "$HTTPS_PORT" false
  fi
  status ok letsencrypt auto_renew_disabled false existing
}
le_renew() {
  [[ -f "$SETTINGS" ]] || exit 0
  # shellcheck disable=SC1090
  source "$SETTINGS"
  le_enable
}
