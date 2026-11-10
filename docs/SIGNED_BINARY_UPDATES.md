# Signed DVBStreamer5 binary updates (V10.8.434)

The root menu exposes **Check for updates** and, only if a signed release is newer, **Install update**. Neither action changes streaming configuration. Installation is **explicit** and temporarily restarts broadcasting.

## Publishing on https://tvstreammersat5.tech/

Publish these files (same HTTPS host, no CDN redirect):

- `/downloads/dvbstreamer5/latest.json`
- `/downloads/dvbstreamer5/latest.json.sig` (detached Ed25519 signature of the **exact JSON bytes**)
- `/downloads/dvbstreamer5/DVBStreamer5-VERSION-linux-x86_64.tar.gz`

Release manifest example:

```json
{
  "version": "10.8.435",
  "url": "https://tvstreammersat5.tech/downloads/dvbstreamer5/DVBStreamer5-10.8.435-linux-x86_64.tar.gz",
  "sha256": "REPLACE_WITH_64_HEX_DIGEST",
  "size": 12345678
}
```

The archive must come from `scripts/make_portable_linux_bundle.sh --archive` and include `bin/DVBStreamer5`, `run.sh`, `install.sh` and `SHA256SUMS`. Publish files atomically: archive first, then signature, then manifest. Never publish a manifest that points to an incomplete upload.

### Signing key (publisher only)

Generate **once** on an offline/trusted publisher machine:

```bash
openssl genpkey -algorithm Ed25519 -out release-private.pem
openssl pkey -in release-private.pem -pubout -out release-public.pem
chmod 0600 release-private.pem
```

**Never put `release-private.pem` in GitHub, a web-accessible directory, or the application server.** Back it up securely.

After preparing a real manifest with a matching version, size, and SHA256:

```bash
openssl pkeyutl -sign -inkey release-private.pem -rawin \
  -in latest.json -out latest.json.sig
```

Upload `latest.json`, `latest.json.sig` and the release archive to the fixed paths above. The `.sig` is binary data, not base64.

### Install trusted public key on each streaming server

Provision the public key over a separately trusted administration channel:

```bash
sudo install -d -m 0700 /etc/dvbstreamer5
sudo install -m 0644 release-public.pem /etc/dvbstreamer5/update-public.pem
```

Install V10.8.434 or later using its normal portable package first. Then in the web UI use **Menu → Check for updates**. A button to install is shown only when the detached signature validates and the advertised version compares greater than the running version.

### Updating and rollback

The updater runs outside the application process, under the installed root systemd service. It verifies the signature using the **locally installed key**, enforces the fixed site and archive filename, compares version tuples, verifies both archive size and SHA256, rejects dangerous tar entries, verifies bundle `SHA256SUMS`, and backs up `/opt/DVBStreamer5` before running the bundled installer.

```bash
sudo tail -n 100 /var/log/dvbstreamer5-update.log
sudo systemctl status dvbstreamer5.service oscam-mini.service --no-pager
sudo ls -lah /var/lib/dvbstreamer5/update-backups
```

When installation fails, it attempts to restore the backed-up runtime and restart the program. Keep the previous portable archive for manual disaster recovery. App configuration lives outside the runtime under `/etc/dvbstreamer5`; do not remove it.

**Security:** the check does not use client-supplied download URLs. The install endpoint is limited to an authenticated browser panel session and explicitly marked same-origin AJAX request; API-key authorization cannot trigger a binary install. Update signing is mandatory. No background auto-install, no silent upgrades, and no version downgrade.
