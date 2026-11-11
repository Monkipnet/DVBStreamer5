# DVBStreamer5 binary updates (V10.8.436)

The updater is a Bash shell script, not Python: scripts/dvbstreamer5_update.sh.
It uses the fixed website download URL:

https://tvstreammersat5.tech/download.php?id=5c15e7ab4ff1c26daef79d745d211c3d

In Menu, click **Check for updates**. The **Install update** button becomes visible if the advertised version is greater than the running version. Installation needs explicit confirmation; it interrupts live streaming.

## Release version discovery

Preferred (unsigned) metadata URL:

https://tvstreammersat5.tech/downloads/dvbstreamer5/latest.json

Example (replace placeholders with actual binary archive details):

```json
{
  "version": "10.8.437",
  "url": "https://tvstreammersat5.tech/download.php?id=5c15e7ab4ff1c26daef79d745d211c3d",
  "sha256": "REPLACE_WITH_64_CHARACTER_SHA256",
  "size": 12345678
}
```

Bash requires the `jq` utility only for parsing this JSON metadata. Without metadata it reads the HTTP attachment filename from the `Content-Disposition` header of the download endpoint. The filename must resemble `DVBStreamer5-10.8.437-linux-x86_64.tar.gz`. Without either a valid JSON version or versioned attachment name, the updater declines to proceed.

## Manual checks

From the installed server:

```bash
bash /opt/DVBStreamer5/updater.sh check 10.8.436
sudo tail -n 100 /var/log/dvbstreamer5-update.log
systemctl status dvbstreamer5.service oscam-mini.service --no-pager
```

The updater accepts only the built-in download URL, limits archive size, checks an optional SHA256 digest and the internal SHA256SUMS, inspects archive paths and types, backs up the runtime under `/var/lib/dvbstreamer5/update-backups`, installs with the bundle installer, and attempts to restore the runtime if installation fails.

**Temporary security exception:** release signatures and public keys are *not* required. HTTPS and SHA256 values published by the same site cannot establish publisher authenticity if the website is compromised. For production deployments, prefer restoring signed release manifests when the publishing infrastructure is ready. Do not run unattended updates. Configuration in `/etc/dvbstreamer5` is not intentionally overwritten by the installer.

## V10.8.464: fix service-killed web updater

The former `nohup updater.sh install ... &` ran in the systemd cgroup of
`dvbstreamer5.service`. The bundled installer stops that very service;
systemd then kills the updater along with its parent. This exactly fits
the incident where V454 stopped but its files were not updated to V463.

The web launcher now uses `/usr/bin/systemd-run --no-block` to create a
uniquely named independent transient system service. Update output
continues in `/var/log/dvbstreamer5-update.log`. Failure to spawn the
worker returns an explicit HTTP error without an unsafe `nohup` fallback.

The Bash updater now restores the previous runtime and service unit files
on errors after installation has started and restarts OSCam-mini plus
DVBStreamer5. It checks both runtime version and service activity.

**Already installed V454 cannot fix its broken web launcher through the
same web route.** Do one manual, checksum-verified portable installation
from a separate SSH/root session. The installer interrupts streams, so
perform it during a planned maintenance window. Preserve a v454 backup.
