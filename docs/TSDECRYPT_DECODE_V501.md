# DVBStreamer5 V10.8.501 — TSDecrypt decode evidence and portable runtime

## Symptom (home2, Ubuntu 18.04 test host)

Several channels showed the green **ДЕКОД: ОК** label but their media
could not be viewed. V500 /api/state marked a tile clear whenever
output scrambling bits were zero and at least one plausible PES start
appeared. The TSDecrypt child can clear a scrambling bit without yielding
playable video, and CW/PES events alone do not prove codec decode.

Home2 selected /usr/local/bin/tsdecrypt because the portable
/opt/DVBStreamer5/bin/tsdecrypt wrapper was absent. That binary had no
observable "Holding recently valid ECM pid" policy marker. Some SIDs
had repeated card-denied responses and ECM PID rotation; SID 2027
had no clear video PES in a five-minute sample.

## V501 implementation

- For explicitly configured TSDecrypt clients only, map nominal clear
  output to `ca_decode_state=unverified`; render neutral "ДЕКОД: ?"
  with text indicating that playback was not verified. Do not return
  "ДЕКОД: ОК" from TSDecrypt merely on CW/PES/scrambling bits.
- Disable the six-second green-state hold only for TSDecrypt.
  All non-TSDecrypt CA state handling is unchanged.
- Recover TSDecrypt's exact pinned upstream libfuncs/libtsfuncs commits
  from the author's GitHub mirrors. Their URLs in upstream .gitmodules
  refer to a retired host; local git -c overrides avoid editing upstream
  source manifests and preserve commit pinning.
- Include build/tsdecrypt-icam/tsdecrypt preferentially in portable bundles;
  --with-tsdecrypt explicitly fails packaging if absent. Main Linux CI builds
  the modified TSDecrypt and uploads a package with its launcher and binary.

## Limits and rollout

This release corrects misleading status and makes the intended modified
TSDecrypt deployable. It does NOT claim to repair reader rejection, unknown
entitlement, CSA-ALT compatibility or prove playable video merely from PES.
No video preview, native transcoding, output protocols, Newcamd, OSCam-mini,
CI+, BISS, or service authentication logic is changed.

Build on the source tree's Linux host:

```bash
git switch main && git pull --ff-only origin main
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --parallel 2
ctest --test-dir build-release --output-on-failure
git clone https://github.com/gfto/tsdecrypt.git /tmp/dvbstreamer5-tsdecrypt
git -C /tmp/dvbstreamer5-tsdecrypt checkout --detach f4876e84cf1866645f84b93f830af67193c85f69
bash scripts/build_tsdecrypt_icam.sh /tmp/dvbstreamer5-tsdecrypt build-release
bash scripts/make_portable_linux_bundle.sh --build-dir build-release \
  --with-oscam --with-tsdecrypt \
  --output-dir dist/DVBStreamer5-10.8.501-linux-x86_64 --archive
```

On home2, back up /opt/DVBStreamer5, /etc/dvbstreamer5 and existing
systemd units before intentionally restarting services. Verify a single
authorized SID using independently decoded video/audio and playback rather
than relying on status text. Keep the V500 bundle as the rollback baseline.
