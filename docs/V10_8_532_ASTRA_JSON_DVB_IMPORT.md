# V10.8.532 — Cesbo Astra JSON DVB-S/S2/T/T2 import

Date: 2026-11-15. Branch: `Monkipnet/DVBStreamer5 main`.

## Problem / source evidence

In V10.8.531, the Cesbo Astra JSON importer
`src/http/ExternalConfigTransfer.h::parseAstra()` ignored
`dvb_tune`, rejected `dvb://<tuner_id>#pnr=...` as unsupported,
and treated HTTP output URLs as unsupported. It also discarded the
original channel `make_stream[].id` when assigning internal safe
`migration_astra_...` IDs.

The operator supplied two Astra JSON exports (credentials omitted
from this document and test fixtures). Both **original source
files are Windows-1251 encoded**:

| Export | `dvb_tune` entries | `make_stream` SPTS entries | Referenced tune IDs | HTTP output entries |
|---|---:|---:|---:|---:|
| Satellite | 13 (11 S2, 2 S) | 174 | 10 | 99 |
| Terrestrial | 4 T2 | 38 | 4 | 26 |
| Total | 17 | 212 | 14 | 125 |

- Satellite entries provide Astra tuner IDs, `adapter`, `device`,
  `type`, `frequency` (including e.g. `11785.02` MHz),
  `symbolrate`, `polarization`, LOF1/LOF2/SLOF, DiSEqC.
- T2 entries use `frequency` in MHz and `bandwidth` strings
  `7MHz` and `8MHz`. `dvb://a0c5#pnr=2` is ordinary DVB-T2,
  not T2-MI.
- Original `make_stream[].id` values such as `a005` are stable
  TV channel identities for `tvg-id`; they are NOT DVB PNRs.
- Typical Astra HTTP outputs `http://host:5555/channel` are
  HTTP MPEG-TS **server outputs**, not HTTP inputs or outbound POSTs.

## V532 implementation

1. New `src/http/AstraJsonDvbTune.h`: bounded JSON tuner
   dictionary; resolves symbolic `dvb://<id>#pnr=N&cam=...`
   into native DVBStreamer5 `dvb://satellite?` or
   `dvb://terrestrial?` URI.
   Converts Astra MHz values to native kHz and handles fractional
   satellite frequencies, DVB-S/S2/T/T2, adapter/frontend, SR,
   polarity, standard LOF and DiSEqC, bandwidth (5/6/7/8/10 MHz),
   optional DVB-T2 PLP and DVB-S2 ISI.
   PNR is stored as `input_service_id` **separately from the
   tuner-specific ISI/PLP**.
2. The importer remains 4 MiB bounded. It decodes invalid UTF-8
   as Windows-1251 via libc iconv, with an explicit warning.
   `src/http/HttpServerWebUi.inc` now reads Astra/Flussonic
   files as a byte buffer: strict UTF-8 decoding, then Windows-1251
   fallback. Otherwise browser `File.text()` silently replaces
   non-UTF8 channel titles before C++ receives them.
3. HTTP outputs become `output_type=http`,
   `output_mode=listener`, `output_port=0` (existing
   DVBStreamer5 HTTP listener). **Astra's original HTTP server
   IP, port and path are not preserved**; the new public path is
   `http://<DVBStreamer5-IP>:<configured-http-port>/<migration_astra_...>/mpegts`.
   Imported Astra channels use their unique internal tile ID for their
   public HTTP/HLS URL slug because duplicate or Cyrillic channel names
   otherwise collapse to the same path. Human-readable display names and
   original Astra `tvg-id` remain separate. This intentionally avoids
   binding someone else's IP/occupied old
   Astra port and does not restart the current media pipeline.
   Existing UDP/RTP outputs and additional outputs remain available.
4. Added `StreamConfig::tvgId` JSON field `tvg_id`, populated
   from `make_stream[].id`. For M3U `#EXTINF tvg-id` and
   locally generated XMLTV channel IDs, it overrides only the
   advertised EPG identity; **internal tile ID and MPEG-TS PNR/SID
   stay independent**. Existing tiles with empty `tvg_id` keep
   their previous M3U/EPG identities. The channel editor exposes
   and preserves this value.
5. `mergeNewOnly()` still appends unique tiles, never overwrites
   existing production services, and forces `auto_start=false`.
   It does not import sensitive `softcam`, login, auth,
   subscribers, network settings, CA credentials, or Astra processes.
6. T2MI **is not implemented in DVBStreamer5 native media input**.
   The JSON converter detects `t2mi://` and emits
   `T2MI input skipped: native T2-MI PLP/TS depacketizer is
   unavailable`. It never silently maps T2MI to DVB-T2: doing so
   would deliver the **outer** satellite transport stream instead
   of the intended encapsulated service. Neither of the supplied
   examples contains a `t2mi://` input. A separate packet-level
   T2-MI extractor/PLP selector is needed for full runtime support.
7. Unicable fields with **nonempty** `uni_scr`, `uni_frequency`
   or `uni_pos` are not guessed; such tuner references are skipped
   with a warning to prevent retuning an active shared UB.
   Empty strings (seen in the provided satellite export) are ignored.

## Verification

- `tests/test_external_config_transfer.cpp`:
  synthetic, **credential-free** satellite/T2 JSON examples
  validate 11785.02 MHz, LOF/SR/polarity, 626 MHz/8MHz,
  205 MHz/7MHz, PNR to `input_service_id`, Astra ID to
  `tvg_id` round-trip, HTTP output to server port, repeated
  import deduplication, disabled auto-start, CA warning, T2MI
  explicit refusal, and Windows-1251 to UTF-8.
  Existing Lua Astra 4.4, Flussonic and UDP tests remain.
- `tests/web/test_astra_json_dvb_v532.js` verifies the
  native importer/HTTP/EPG persistence/browser encoding wiring.
  Registered in **both** main Linux workflow validation sections.
- Code consistency checks are not a substitute for a successful
  complete CMake/CTest, Actions artifact and actual receiver test.
  Real T2MI playback is **NOT** claimed.

## Operator import

1. Back up `/etc/dvbstreamer5/dvbstreamer5-config.json`,
   `/etc/dvbstreamer5/dvbstreamer5-ui.key`, the portable
   installed V531 archive, and confirm maintenance window for installation.
2. Build/install V532 on a test server first. Do not upgrade a
   working production server blindly.
3. Web panel → Administration → Configuration → Import format:
   **Cesbo Astra 5.x JSON** (`astra`).
4. Select the original satellite/terrestrial JSON file;
   **Preview import**. Check `new_streams`, `ignored`, `warnings`.
   Preview does not start/retune any DVB adapters.
5. Confirm the converted tuner adapter numbers refer to **actual**
   `/dev/dvb/adapterN/frontendM` devices on the target server;
   do NOT assume Astra's hardware numbering is identical.
6. Apply only when satisfied. Imported tiles remain **stopped**
   even when Astra says `enable=true`.
7. Inspect one imported satellite and one DVB-T2 tile manually:
   tuner URI, PNR, LNB/DiSEqC, bandwidth and PLP, HTTP output
   and `tvg-id`. Start only one **unoccupied** tuner at a time.
   Do not retune a frontend with existing service consumers.
8. Test the HTTP TS URL displayed by DVBStreamer5 in VLC;
   verify `#EXTINF tvg-id=\"<Astra-ID>\"` in
   `/playlist.m3u`.
9. On regressions, stop only the newly imported test tiles and
   restore the verified configuration/portable backup.

## Source-root build

```bash
git switch main
git pull --ff-only origin main
node tests/web/test_external_config_transfer_ui.js
node tests/web/test_astra_json_dvb_v532.js
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --parallel 2
ctest --test-dir build-release -R '^external_config_transfer$' --output-on-failure
ctest --test-dir build-release --output-on-failure
bash scripts/make_portable_linux_bundle.sh --build-dir build-release --with-oscam --archive
(cd dist && sha256sum -c DVBStreamer5-10.8.532-linux-x86_64.tar.gz.sha256)
```

Packaging and installation invoke the normal service-wide shutdown and
restart; do not do it during an unscheduled broadcast window.
