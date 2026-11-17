# V10.8.561 — portable OSCam ECM diagnostics

V560 added key-free opt-in Newcamd ingress, upstream queue/send,
reply/late-reply and client-timeout measurements. V561 ensures the
Python 3.6-compatible analyzer is installed with the portable bundle:

- `scripts/make_portable_linux_bundle.sh` includes
  `scripts/diagnose_oscam_ecm_latency.py`.
- `install.sh` copies `scripts/` to `/opt/DVBStreamer5/scripts/`
  without modifying `/etc/dvbstreamer5/oscam-mini` credentials.
- GitHub Actions checks the analyzer is executable in portable archive.
- The analyzer is usable on home2 Ubuntu 18.04:
  `journalctl -u oscam-mini.service --since '-5 min' -o cat --no-pager | python3 /opt/DVBStreamer5/scripts/diagnose_oscam_ecm_latency.py`.

The diagnostic remains opt-in: set
`DVBSTREAMER5_OSCAM_ECM_LATENCY=1` on OSCam-mini's systemd unit only
during an approved maintenance window. No implicit service restart.
