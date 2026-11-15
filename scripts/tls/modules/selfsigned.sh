#!/usr/bin/env bash
# V546 independent self-signed certificate generation module.
set -Eeuo pipefail
: "${DOMAIN:?}" "${BACKEND_PORT:?}" "${HTTPS_PORT:?}"
require_command openssl
MODE=selfsigned
AUTO_RENEW=false
ISSUER=self_signed
nginx_preflight "$HTTPS_PORT"
status running "$MODE" generating false "$ISSUER"
mkdir -p "$TLS_DIR/selfsigned"
chmod 0700 "$TLS_DIR/selfsigned"
# Generate only in a new temporary directory; failed operations never replace
# the active certificate/key and never delete previous live certificate files.
WORK="$(mktemp -d "$TLS_DIR/selfsigned/.new.XXXXXXXX")"
openssl req -x509 -newkey rsa:3072 -sha256 -nodes -days 365 \
  -keyout "$WORK/privkey.pem" -out "$WORK/fullchain.pem" \
  -subj "/CN=$DOMAIN" -addext "subjectAltName=DNS:$DOMAIN" >/dev/null 2>&1 ||
  { rm -rf -- "$WORK"; die_tls openssl_generation_failed; }
chmod 0600 "$WORK/privkey.pem"
chmod 0644 "$WORK/fullchain.pem"
NEW_DIR="$TLS_DIR/selfsigned/$(date +%s)-$$"
mv "$WORK" "$NEW_DIR"
activate_certificate "$NEW_DIR/fullchain.pem" "$NEW_DIR/privkey.pem"
nginx_managed_proxy "$DOMAIN" "$BACKEND_PORT" "$HTTPS_PORT" false
# Explicit manual self-signed switch disables automatic LE updates.
systemctl disable --now dvbstreamer5-le-renew.timer >/dev/null 2>&1 || true
rm -f -- "$SETTINGS"
status ok selfsigned selfsigned_certificate_active false self_signed
