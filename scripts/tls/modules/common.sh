#!/usr/bin/env bash
# DVBStreamer5 V546: common TLS module, used exclusively by local admin operations.
set -Eeuo pipefail
umask 077
TLS_DIR=/etc/dvbstreamer5/tls
STATE=$TLS_DIR/state.json
SETTINGS=$TLS_DIR/letsencrypt.env
ACME_ROOT=/var/lib/dvbstreamer5/acme
NGINX_SITE=/etc/nginx/conf.d/dvbstreamer5-managed-tls.conf
MODULE_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
mkdir -p "$TLS_DIR" "$ACME_ROOT/.well-known/acme-challenge"
chmod 0700 "$TLS_DIR"
status() {
  local phase="$1" mode="$2" message="$3" enabled="${4:-false}" issuer="${5:-none}"
  local domain="${DOMAIN:-}" expiry=''
  if [[ -r "$TLS_DIR/active.crt" ]]; then
    expiry="$(openssl x509 -in "$TLS_DIR/active.crt" -noout -enddate 2>/dev/null | sed 's/^notAfter=//' || true)"
  fi
  printf '{"phase":"%s","mode":"%s","domain":"%s","message":"%s","auto_renew":%s,"issuer":"%s","expires":"%s"}\n' \
    "$phase" "$mode" "$domain" "$message" "$enabled" "$issuer" "$expiry" >"$STATE.tmp.$$"
  chmod 0600 "$STATE.tmp.$$"
  mv -f -- "$STATE.tmp.$$" "$STATE"
}
die_tls() {
  status error "${MODE:-none}" "$1" "${AUTO_RENEW:-false}" "${ISSUER:-none}"
  echo "DVBStreamer5 TLS: $1" >&2
  exit 1
}
check_domain() {
  local name="$1" label
  [[ ${#name} -le 253 && ${#name} -ge 4 && "$name" == *.* &&
     "$name" =~ ^[A-Za-z0-9.-]+$ && "$name" != *..* ]] || die_tls invalid_domain
  local IFS=.
  read -ra labels <<< "$name"
  for label in "${labels[@]}"; do
    [[ ${#label} -ge 1 && ${#label} -le 63 &&
       "$label" =~ ^[A-Za-z0-9]([A-Za-z0-9-]*[A-Za-z0-9])?$ ]] || die_tls invalid_domain
  done
  [[ "$name" != localhost.* && "$name" != *.local && "$name" != *.internal ]] ||
    die_tls invalid_domain
}
check_port() {
  local p="$1"
  [[ "$p" =~ ^[1-9][0-9]{0,4}$ ]] && ((10#$p <= 65535)) || die_tls invalid_port
}
check_email() {
  local email="$1"
  [[ ${#email} -le 254 &&
     "$email" =~ ^[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}$ ]] ||
    die_tls invalid_email
}
require_command() {
  command -v "$1" >/dev/null 2>&1 || die_tls "missing_$1"
}
ensure_nginx() {
  if ! command -v nginx >/dev/null 2>&1; then
    command -v apt-get >/dev/null 2>&1 || die_tls nginx_not_installed
    status running "${MODE:-selfsigned}" installing_nginx "${AUTO_RENEW:-false}" none
    DEBIAN_FRONTEND=noninteractive apt-get update -qq &&
    DEBIAN_FRONTEND=noninteractive apt-get install -y -qq nginx ||
      die_tls nginx_install_failed
  fi
  require_command nginx
}
# CA support: retain the previous active certificate on ACME request failure.
nginx_managed_proxy() {
  local domain="$1" backend="$2" https_port="$3" acme="${4:-false}"
  ensure_nginx
  if [[ -e "$NGINX_SITE" && ! -e "$TLS_DIR/nginx-managed" ]]; then
    die_tls nginx_config_unmanaged
  fi
  local candidate
  candidate="$(mktemp /etc/nginx/conf.d/.dvbstreamer5-tls.XXXXXXXX)"
  cat > "$candidate" <<EOF
# DVBStreamer5 managed HTTPS proxy — do not edit; use Control Panel.
server {
    listen ${https_port} ssl;
    server_name ${domain};
    ssl_certificate ${TLS_DIR}/active.crt;
    ssl_certificate_key ${TLS_DIR}/active.key;
    ssl_protocols TLSv1.2 TLSv1.3;
    client_max_body_size 512m;
    proxy_request_buffering off;
    location / {
        proxy_pass http://127.0.0.1:${backend};
        proxy_http_version 1.1;
        proxy_set_header Host \$http_host;
        proxy_set_header X-Real-IP \$remote_addr;
        proxy_set_header X-Forwarded-For \$remote_addr;
        proxy_set_header X-Forwarded-Proto https;
        proxy_read_timeout 3600s;
        proxy_buffering off;
    }
}
EOF
  if [[ "$acme" == true ]]; then
    cat >> "$candidate" <<EOF
# ACME HTTP-01 requires public TCP/80. Existing virtual hosts are unchanged.
server {
    listen 80;
    server_name ${domain};
    location ^~ /.well-known/acme-challenge/ {
        root ${ACME_ROOT};
        try_files \$uri =404;
    }
    location / { return 404; }
}
EOF
  fi
  local backup=''
  if [[ -f "$NGINX_SITE" ]]; then
    backup="$(mktemp /etc/nginx/conf.d/.dvbstreamer5-old.XXXXXXXX)"
    cp -p "$NGINX_SITE" "$backup"
  fi
  mv -f "$candidate" "$NGINX_SITE"
  if ! nginx -t >/dev/null 2>&1; then
    if [[ -n "$backup" ]]; then mv -f "$backup" "$NGINX_SITE"; else rm -f "$NGINX_SITE"; fi
    die_tls nginx_config_invalid
  fi
  rm -f "$backup"
  touch "$TLS_DIR/nginx-managed"
  systemctl enable --now nginx >/dev/null 2>&1 || die_tls nginx_start_failed
  systemctl reload nginx >/dev/null 2>&1 || die_tls nginx_reload_failed
}
activate_certificate() {
  local cert="$1" key="$2"
  [[ -s "$cert" && -s "$key" ]] || die_tls missing_certificate
  openssl x509 -in "$cert" -noout -checkend 86400 >/dev/null 2>&1 ||
    die_tls certificate_expired
  openssl pkey -in "$key" -noout >/dev/null 2>&1 || die_tls invalid_private_key
  local prior_cert='' prior_key=''
  [[ -L "$TLS_DIR/active.crt" ]] && prior_cert="$(readlink "$TLS_DIR/active.crt")"
  [[ -L "$TLS_DIR/active.key" ]] && prior_key="$(readlink "$TLS_DIR/active.key")"
  ln -sfn "$cert" "$TLS_DIR/active.crt"
  ln -sfn "$key" "$TLS_DIR/active.key"
  if [[ -e "$NGINX_SITE" ]] && ! nginx -t >/dev/null 2>&1; then
    if [[ -n "$prior_cert" ]]; then ln -sfn "$prior_cert" "$TLS_DIR/active.crt"; else rm -f "$TLS_DIR/active.crt"; fi
    if [[ -n "$prior_key" ]]; then ln -sfn "$prior_key" "$TLS_DIR/active.key"; else rm -f "$TLS_DIR/active.key"; fi
    die_tls activation_failed
  fi
}