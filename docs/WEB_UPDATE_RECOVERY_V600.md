# DVBStreamer5 V10.8.600 — repair the existing website update flow

## Why the updater can appear broken

The main Linux GitHub Actions workflow uploads a versioned artifact **to GitHub**
but does **not** publish the archive or `latest.json` at
`https://tvstreammersat5.tech/downloads/dvbstreamer5/`. The website and the
GitHub Actions artifact are different release channels. A newly built main
binary is **not** an already published downloadable update.

Old updater scripts require `jq` when `latest.json` is present and accept
only a fixed `download.php?id=...` URL. The V10.8.600 script supports
`python3` in the absence of `jq` and also accepts exactly a version-matched
static archive at the official HTTPS release directory.

It **does not** accept arbitrary metadata URLs or trust the website header
to verify the actual binary archive. The installer still validates versions,
checksums, archive members, service restart and rollback.

## 1 — Read-only diagnosis on home2 (V10.8.589)

```bash
cat /opt/DVBStreamer5/VERSION
command -v jq || echo 'jq missing (old updater cannot parse JSON)'
command -v python3
ls -l /opt/DVBStreamer5/updater.sh
bash /opt/DVBStreamer5/updater.sh check "$(cat /opt/DVBStreamer5/VERSION)"
curl -fsSL --connect-timeout 10 --max-time 20 \
  https://tvstreammersat5.tech/downloads/dvbstreamer5/latest.json
echo
systemctl status dvbstreamer5.service --no-pager
```

These commands do not restart DVBStreamer5 or alter channel state.

## 2 — Build and verify the portable artifact in GitHub Actions

Use the `build-main-linux.yml` run for the **exact** source commit and confirm
successful CI completion, the archive `VERSION` and `sha256sum -c`. Do not
publish an artifact from a failed or unrelated run. TSDecrypt and OSCam-mini
are mandatory in the standard portable packaging.

## 3 — Publish on the website host (authorized website owner)

Copy the verified `DVBStreamer5-10.8.600-linux-x86_64.tar.gz` to the website
host, as well as `scripts/publish_portable_release.sh` from the same source
commit. Run:

```bash
bash scripts/publish_portable_release.sh \
  /path/to/DVBStreamer5-10.8.600-linux-x86_64.tar.gz \
  /home/WEB_ACCOUNT/web/tvstreammersat5.tech/public_html
```

Replace `WEB_ACCOUNT` with the real HestiaCP web account. This is **not**
a public upload endpoint. The script checks the archive and embedded version,
publishes a static HTTPS file, then atomically writes matching `latest.json`
containing version, exact URL, SHA256 and byte size. Existing releases remain
available for rollback.

Verify with `curl -fsSL` and `curl -I` that the public URLs answer and that
the `sha256` matches the archive on the website server. Some HestiaCP rules may
block this path: verify HTTP 200 for the static archive, not merely the JSON.

## 4 — Restore the existing home2 updater without touching live streams

After the website publishes a verified version, copy
`scripts/dvbstreamer5_update.sh` from a trusted, reviewed main checkout to
the home2 server and replace only its installed updater shell script:

```bash
bash -n /tmp/dvbstreamer5_update.sh
cp -a /opt/DVBStreamer5/updater.sh \
  "/opt/DVBStreamer5/updater.sh.before-v600.$(date +%Y%m%d%H%M%S)"
install -m 0755 /tmp/dvbstreamer5_update.sh /opt/DVBStreamer5/updater.sh
bash /opt/DVBStreamer5/updater.sh check 10.8.589
```

No streaming service is stopped by these four commands. Do not run
`updater.sh install` or click `Install update` until the release archive
and its GitHub Actions tests are confirmed; installation restarts streams.

## CI scope

`tests/web/test_update_metadata_compat_v599.js` emulates Ubuntu without
`jq` using Python 3, and both the original fixed-PHP and exact-version
static-archive URLs, rejecting mismatches and foreign hosts.

`tests/web/test_update_publication_v599.js` checks website publication,
metadata SHA256, and refusal of mismatched archive names/embedded versions.

Hardware playback and real website deployment require separate verification.
