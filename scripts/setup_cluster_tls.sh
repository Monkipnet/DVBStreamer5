#!/usr/bin/env bash
# V541: unattended TLS reverse proxy for the cluster/UI HTTP listener.
# Usage: sudo bash scripts/setup_cluster_tls.sh --domain node.example.org --backend-port 9000 --https-port 9443 --email admin@example.org
set -Eeuo pipefail
umask 077
DOMAIN='' EMAIL='' BACKEND_PORT=9000 HTTPS_PORT=9443
while (($#)); do
 case "$1" in
 --domain) DOMAIN="${2:?}"; shift 2;;
 --email) EMAIL="${2:?}"; shift 2;;
 --backend-port) BACKEND_PORT="${2:?}"; shift 2;;
 --https-port) HTTPS_PORT="${2:?}"; shift 2;;
 *) echo "Unsupported option: $1" >&2; exit 2;;
 esac
done
[[ ${EUID} -eq 0 ]] || { echo 'Root required' >&2; exit 2; }
[[ "$DOMAIN" =~ ^[a-zA-Z0-9]([a-zA-Z0-9.-]{0,251}[a-zA-Z0-9])?$ && "$DOMAIN" == *.* && "$DOMAIN" != *..* ]] || { echo 'Valid DNS hostname required' >&2; exit 2; }
for port in "$BACKEND_PORT" "$HTTPS_PORT"; do
 [[ "$port" =~ ^[0-9]{1,5}$ ]] && ((10#$port > 0 && 10#$port < 65536)) || { echo 'Invalid port' >&2; exit 2; }
done
[[ "$BACKEND_PORT" != "$HTTPS_PORT" ]] || { echo 'Proxy and application ports must differ' >&2; exit 2; }
# This only binds HTTPS. It never changes the original HTTP/UDP/SRT ports.
apt-get update
DEBIAN_FRONTEND=noninteractive apt-get install -y nginx openssl certbot ca-certificates
DIR="/etc/dvbstreamer5/tls"
install -d -m 0700 "$DIR"
CERT="$DIR/fullchain.pem"
KEY="$DIR/privkey.pem"
LE_CERT="/etc/letsencrypt/live/$DOMAIN/fullchain.pem"
LE_KEY="/etc/letsencrypt/live/$DOMAIN/privkey.pem"
have_valid_le=0
if [[ -r "$LE_CERT" && -r "$LE_KEY" ]] && openssl x509 -in "$LE_CERT" -noout -checkend 604800 >/dev/null 2>&1; then
 have_valid_le=1
fi
# Port 80 must be reachable by the CA for HTTP-01. Failure is non-fatal.
if [[ -n "$EMAIL" ]]; then
 if certbot certonly --standalone --preferred-challenges http --non-interactive --agree-tos \
      --email "$EMAIL" --keep-until-expiring -d "$DOMAIN"; then
   have_valid_le=1
 else
   echo 'Let\x27s Encrypt unavailable; checking existing certificates and self-signed fallback' >&2
 fi
fi
if [[ -r "$LE_CERT" && -r "$LE_KEY" ]] &&
   openssl x509 -in "$LE_CERT" -noout -checkend 604800 >/dev/null 2>&1; then
 have_valid_le=1
fi
if (( have_valid_le )); then
 ln -sfn "$LE_CERT" "$CERT"
 ln -sfn "$LE_KEY" "$KEY"
 echo "Using verified Let's Encrypt certificate for $DOMAIN"
else
 if [[ ! -s "$CERT" || ! -s "$KEY" ]] ||
    ! openssl x509 -in "$CERT" -noout -checkend 2592000 >/dev/null 2>&1; then
   rm -f "$CERT" "$KEY"
   openssl req -x509 -newkey rsa:3072 -sha256 -nodes -days 90 \
      -keyout "$KEY" -out "$CERT" -subj "/CN=$DOMAIN" \
      -addext "subjectAltName=DNS:$DOMAIN"
   chmod 0600 "$KEY"
 fi
 echo 'SELF-SIGNED fallback: browsers and remote cluster peers will NOT trust this certificate until the certificate/CA is explicitly trusted.' >&2
fi
CONFIG=/etc/nginx/conf.d/dvbstreamer5-cluster-tls.conf
# Do not overwrite a custom nginx proxy configuration.
if [[ -e "$CONFIG" && ! -f /etc/dvbstreamer5/tls/proxy-managed ]]; then
 echo "Existing unmanaged nginx config at $CONFIG; refusing overwrite" >&2; exit 1
fi
cat > "$CONFIG" <<EOF
# Managed by DVBStreamer5 TLS setup; use a unique external HTTPS port.
server {
    listen ${HTTPS_PORT} ssl;
    server_name ${DOMAIN};
    ssl_certificate ${CERT};
    ssl_certificate_key ${KEY};
    ssl_protocols TLSv1.2 TLSv1.3;
    client_max_body_size 32m;
    proxy_buffering off;
    proxy_request_buffering off;
    proxy_read_timeout 3600s;
    location / {
        proxy_pass http://127.0.0.1:${BACKEND_PORT};
        proxy_http_version 1.1;
        proxy_set_header Host \\$host;
        proxy_set_header X-Real-IP \\$remote_addr;
        proxy_set_header X-Forwarded-For \\$proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto https;
    }
}
EOF
nginx -t
touch /etc/dvbstreamer5/tls/proxy-managed
systemctl enable --now nginx
systemctl reload nginx
# Persist a safe root-only argument file for the daily automatic refresh.
printf 'DOMAIN=%q\nEMAIL=%q\nBACKEND_PORT=%q\nHTTPS_PORT=%q\n' "$DOMAIN" "$EMAIL" "$BACKEND_PORT" "$HTTPS_PORT" > /etc/dvbstreamer5/tls/settings
chmod 0600 /etc/dvbstreamer5/tls/settings
install -m 0700 "${BASH_SOURCE[0]}" /usr/local/sbin/dvbstreamer5-cluster-tls
cat > /etc/systemd/system/dvbstreamer5-cluster-tls.service <<'EOF'
[Unit]
Description=DVBStreamer5 cluster TLS certificate refresh
After=network-online.target
Wants=network-online.target
[Service]
Type=oneshot
ExecStart=/usr/local/sbin/dvbstreamer5-cluster-tls-refresh
EOF
cat > /usr/local/sbin/dvbstreamer5-cluster-tls-refresh <<'EOF'
#!/usr/bin/env bash
set -Eeuo pipefail
source /etc/dvbstreamer5/tls/settings
exec /usr/local/sbin/dvbstreamer5-cluster-tls --domain "$DOMAIN" --email "$EMAIL" --backend-port "$BACKEND_PORT" --https-port "$HTTPS_PORT"
EOF
chmod 0700 /usr/local/sbin/dvbstreamer5-cluster-tls-refresh
cat > /etc/systemd/system/dvbstreamer5-cluster-tls.timer <<'EOF'
[Unit]
Description=Daily DVBStreamer5 TLS certificate renewal check
[Timer]
OnCalendar=daily
RandomizedDelaySec=1h
Persistent=true
[Install]
WantedBy=timers.target
EOF
systemctl daemon-reload
systemctl enable --now dvbstreamer5-cluster-tls.timer
echo "TLS endpoint: https://$DOMAIN:$HTTPS_PORT (application continues at HTTP port $BACKEND_PORT)"
