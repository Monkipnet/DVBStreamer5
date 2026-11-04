## V10.8.337 — Fix HTTP/HLS freezes after transcoding without changing working SRT

- **Reported behavior:** after transcoding, SRT is smooth but HTTP MPEG-TS and HLS periodically freeze. Preserve the working codec/mux/SRT transport and investigate these delivery-specific outputs independently.
- **HTTP MPEG-TS producer gaps:** the production `NativePreviewHub` previously drained its pending TS queue only from `publish()`. When a live encoder paused while backlog existed, a TCP reader could drain the local socket and then block indefinitely even though previously queued packets were available in user space. Introduce a nonblocking, per-subscriber `flushPending(readFd)` and invoke it from the dedicated HTTP writer after each successful TCP write; every queued TS datagram remains in original order. The hub's 4-MiB per-subscriber limit, backpressure isolation and no-automated-restart behavior remain intact.
- **HLS IDR boundary:** independent transcoded HLS segments must start on valid random-access frames; do not force a mid-GOP cut to manufacture a shorter duration. Instead, when the encoder delays the next IDR, advertise the actual PCR duration in `#EXTINF` up to a bounded 60-second guard instead of truncating the duration to `3 × configured target` and shifting media timeline. Non-independent passthrough segmentation retains its previous cap. Log `NATIVE HLS SEGMENT DELAY` when a transcoded segment takes more than 1.5× the nominal duration (except the very first).
- **Fan-out ordering:** publish already-finished TS to SRT/RTSP/RTMP, public HTTP and MPTS before blocking synchronous HLS filesystem/playlist operations. This does not change packet contents, PTS/DTS/PCR, encoder FPS/GOP or SRT queue settings. Report the first failed HLS segmenter push as `NATIVE HLS OUTPUT FAILED stream_id=... error=...`, rather than silently ignoring it.
- **Regression:** `media_core_tests` now pushes 16,000 ordered TS packets to a slow HTTP-hub subscriber, stops publishing new data, and checks that the consumer can still drain the exact original payload using `flushPending`. Run the media core tests and the full release build before any production rollout; no server or tiles were restarted by this change.
- **Suggested verification:** on one manually restarted test tile, check `NATIVE HTTP TS HUB backlog`, `NATIVE HTTP TS CLIENT detach`, `NATIVE HTTP TS TCP STATUS`, `NATIVE HLS SEGMENT DELAY`, and `NATIVE HLS OUTPUT FAILED`. Compare HTTP and HLS against the known-good SRT using the same encoded service and measure whether the HTTP transport client disconnects or HLS segment duration exceeds the target. If the problem persists, supply a short log from the same timestamp to identify the remaining cause.

## V10.8.336 — Transcoding with CBR: single NULL-stuffing stage and observer startup buffer

- **Issue under investigation:** with both transcoding and CBR enabled, MPEG-TS playback on UDP-CBR and CBR-paced SRT/HTTP can initially lack decodable video; the experimental source-PCR mode may show intermittent freezes. This update addresses two concrete structural contributors. It is not a claim of verified resolution on real satellite/IP feeds.
- **One transport shaping stage:** the production native transcoder's internal `NativeMpegTsMux` now emits PAT/PMT/SDT, encoded video/audio PES and encoder PCR without internal NULL padding (`tc.muxBitrate=0`). Previously `tc.muxBitrate=targetBitrate` generated potentially large media-clock NULL bursts, all of which the downstream `CbrTsPacer` discarded before generating fixed-rate transport NULLs again. Avoiding this duplicate work and burst copying prevents the transcoder's media/output path from paying for redundant padding. The downstream UDP-CBR worker and the observer CBR worker **still** use the configured target bitrate, fixed physical output clock and per-tile CBR mode; output MPEG-TS remains CBR. No video encoder, audio encoder, FPS, GOP, PTS, DTS or PCR preroll is modified.
- **SRT/HTTP-only CBR (no UDP-CBR output):** the observed-output CBR worker now honors the existing per-tile `CBR startup buffer` (default 3000 ms, 0–15000 ms selectable) *for transcoded media only*. The configured delay is capped to keep 30% of its 8-MiB useful-TS queue free, like the WISI UDP sender. Re-anchor the output datagram deadline after collecting the initial reservoir, avoiding a catch-up burst. Existing non-transcoded SRT/HTTP output starts as before; streams that already have a UDP-CBR worker continue sharing its existing independently timed worker, unchanged. The selected buffering values are reported on the `NATIVE OBSERVED CBR start` log line.
- **Per-tile Source-PCR experimental settings:** no new automatic changes to the old PCR source/fixed modes. V334/V335 `ip_source_pcr_passthrough` still applies only without transcoding; the older `Медиатаймлайн по PCR источника` CBR option continues to work as configured. For a first V336 A/B check, test native transcoding with the experimental PCR option OFF, then enable it on only one test tile if necessary.
- **Diagnostics:** `NATIVE TRANSCODER CBR SINGLE SHAPER stream_id=...` confirms the inner mux does not stuff NULL; `NATIVE OBSERVED CBR start ... transcoded=1 requested_startup_ms=... effective_startup_ms=...` confirms startup buffering for observer-only HTTP/SRT CBR. `WISI CBR USEFUL UNDERFLOW`, `NATIVE TRANSCODER CBR BUDGET`, `NATIVE VIDEO ENCODE PERF` and `NATIVE LIVE CADENCE` help distinguish encoder throughput from PCR/queue problems.
- **Regression test:** `transcode_cbr_single_shaper` generates two H.264 frames and AAC with a no-NULL mux, then confirms that the final fixed- and source-PCR CBR pacers fill a complete 7x188-byte datagram with a mixture of real data and NULLs at the original 4000-kbps physical rate. Run `cmake -S tests/media_core -B build-test-media && cmake --build build-test-media --parallel 2 && ctest --test-dir build-test-media -R transcode_cbr_single_shaper --output-on-failure`.
- **Safety:** only the transcoded CBR media path changes; passthrough TS, non-CBR outputs and all Intel/CPU/NVENC encoder settings are preserved. No service, tile or tuner was restarted. A full build and device/client playback test still must be performed by the operator before production deployment.

## V10.8.335 — Fix missing Source-PCR A/B switch in channel editor

- **User-reported bug:** V334 hid the `HTTP/SRT → UDP-CBR Source PCR` checkbox altogether whenever UI eligibility failed. This included SRT channel editing screenshots showing `HTTP TS Jitter Buffer` directly followed by `HTTP / HLS access`, leaving no reason for the absent control.
- **Fix:** move the control next to `Target bitrate (CBR)` and the existing server-injected `CBR startup buffer` / `Source-PCR media timeline` controls. It is **always visible** in the tile edit modal, including on mobile. When ineligible, its checkbox is disabled and an RU/EN/HR message explains whether the cause is input type/HLS, missing UDP-CBR output, CBR disabled, transcoding, test pattern or invalid bitrate. Changing relevant inputs or output options updates the message without losing the saved checkbox value.
- **No streaming change:** the underlying V334 opt-in runtime gate, default OFF, per-stream JSON persistence and no-auto-restart-on-switch-save behaviour remain untouched. After saving the eligible experiment, the operator manually restarts only the selected tile; no other active streams are restarted.
- **Validation:** one `streamIpSourcePcrRow` and one `streamIpSourcePcrEnabled`, next to the CBR bitrate; embedded JavaScript syntax parsed successfully. Browser/runtime verification on the installed V335 binary remains necessary; editing sources alone does not update the running service.

## V10.8.334 — Per-channel GUI switch for HTTP/SRT → UDP-CBR Source-PCR A/B

- **Menu → edit stream tile:** a new per-tile **Source PCR for HTTP/SRT → UDP-CBR** checkbox appears for direct HTTP(S)/SRT MPEG-TS inputs with at least one UDP-CBR output, CBR enabled, test pattern disabled and no transcoding. It is OFF by default; stored as `ip_source_pcr_passthrough` in `/etc/dvbstreamer5/dvbstreamer5-config.json`. Labels and short help are localized RU/EN/HR. The UI preserves an existing preference when other fields make the option temporarily inapplicable; runtime guards still prevent it from activating in that case.
- **No environment variable required:** V333's `DVBSTREAMER5_WISI_SOURCE_PCR_STREAM_ID` selector is replaced by the explicit per-tile setting. The older `DVBSTREAMER5_WISI_SOURCE_PCR_AB` transcoding experiment remains untouched.
- **Physical transport unchanged:** only the selected HTTP/SRT passthrough WISI UDP-CBR worker uses the existing source-PCR timeline. Output transport bitrate remains the configured whole-kbps value; existing CBR startup-buffer selection (0–15,000 ms) is retained using `+799`, `+777` or `+800..829` internal markers. All other tiles, DVB inputs, HLS and hardware/CPU transcode paths preserve existing modes. Use the new switch with the general CBR **Source-PCR media timeline** unchecked for a meaningful A/B comparison.
- **No automatic stream restart for a switch-only Save:** `sameStreamConfig` ignores the new setting for automatic restart comparison, just like EPG metadata. Persist the choice with **Save**, then **manually restart only the selected tile** to reconstruct its CBR worker and apply the change; other active tiles continue running. Editing other stream fields concurrently can retain their normal restart behavior.
- **Tests:** `ctest --test-dir build-test-media -R ip_cbr_source_clock_policy --output-on-failure` exercises default OFF, input/output/transcode guards and preserving physical bitrate + startup buffer for each legacy encoded target marker. Embedded Web UI JavaScript syntax checks can be run independently. Full project build and stream playback need runtime verification on a test channel before adopting the setting broadly. No service was restarted.

## V10.8.333 — HTTP/SRT passthrough → WISI UDP-CBR: selected-source PCR timing A/B

- **Observed symptom:** an IPTV service delivered via HTTP MPEG-TS or SRT shows periodic apparent video speed-up / slow-down while UDP-CBR reports a constant transport bitrate. An exact 4.0 Mbps CBR output alone does **not** prove stable media timestamps. Source transport bitrate fluctuations are expected for VBR and do not establish the cause.
- **Targeted experiment, OFF by default:** set `DVBSTREAMER5_WISI_SOURCE_PCR_STREAM_ID=<exact stream ID>` in the service environment before an approved maintenance restart (or before starting a separate test instance). This applies **only** when the selected stream is an HTTP(S)/SRT MPEG-TS passthrough tile, CBR is enabled, at least one UDP-CBR output exists, and bitrate is in whole kbps. Other tiles, DVB tuner streams, HLS, non-CBR and transcoding retain precisely their prior policy.
- **Mechanism:** selected WISI worker uses existing CbrTsPacer source-timeline `+777` carrier (decoded back to exactly the configured bitrate). In this mode, preserve the incoming PCR unchanged, exactly as PTS/DTS are already preserved, and pace useful packets using the measured source-PCR rate; pad remaining slots with NULL packets. Retain the existing default 3000-ms startup reservoir and disable `+809` WISI reservoir-rate correction for the new test. Legacy +809 transcoding experiment unchanged. Exact selection is logged as `NATIVE WISI SOURCE PCR AB stream_id=...` at stream start. No changes to network sender clock, the other channel configs, GUI, codec or transcoding.
- **Rollback:** unset `DVBSTREAMER5_WISI_SOURCE_PCR_STREAM_ID` and restart the selected test service at an approved time; no persistent config migration is required.
- **Validation:** `cmake -S tests/media_core -B build-test-media && cmake --build build-test-media --parallel 2 && ctest --test-dir build-test-media -R ip_cbr_source_clock_policy --output-on-failure`. This covers opt-in selection gates and +777 physical bitrate/startup-buffer invariants. **Actual stream behaviour and WISI client stability must be measured on a test channel** before any production-wide use. No service restarts have been triggered.

## V10.8.332 — smoother ticker, Unicable cold-lock retries, XMLTV Save closes dialog

- **Ticker:** fixed 22-step speed picker (5–600 px/s, with 10–40 px/s advised for a 25-fps picture). The slider selects the same discrete steps. Previously saved custom integer speeds still load without silently rounding. The on-video ticker keeps its PTS-driven subpixel alpha interpolation; an isolated PTS jump is limited to 1.5 nominal output frame durations to avoid visibly leaping after jitter. Same-PTS repeat remains stationary; saving a speed change preserves the scroll phase. No FPS, video encoder, or PTS/PCR rewrite changes.
- **Unicable EN50494/EN50607:** keep first-tune success paths unmodified. On a later no-LOCK retry, generic PCIe/USB Linux DVB-S/S2 frontends get optional 13V/SEC initialisation and S2API priming, followed by an SCR transaction repeat. The existing TBS `saa716x_tbs_dvb` / `cx23885` STV090x cold-start paths remain enabled. STiD135's driver-owned mode 2 path is preserved; if its mode sysfs file is missing, allow the driver to attempt tuning rather than rejecting solely on unavailable metadata. Persistent no-LOCK is still reported after bounded retries, not disguised as success. This does not guarantee compatibility with every TBS6904/6909 revision, SkyStar PCI/USB receiver or proprietary driver: the adapter must expose working Linux DVB satellite SEC, DiSEqC and frontend operations. Never retune other active frontends automatically.
- **XMLTV:** `Save` closes the sources dialog after a successful `/api/xmltv/save` response, preserves the dialog and entries on error, disables repeated simultaneous submits and avoids closing another dialog after navigation. `Refresh now` remains an independent in-dialog action.
- **Tests/validation:** isolated `unicable_tuning_policy` checks first tune vs recovery for `saa716x_tbs_dvb`, `cx23885`, `stid135` and example generic/USB driver names. Embedded UI JavaScript syntax checked. Full CMake and live RF/NVIDIA/Intel runtime tests must be run on server; no systemd restart was initiated.

## V10.8.331 — Show channel uptime in ONLINE tile status row

- Move the per-channel live running-time clock out of the tile header center and directly next to the **ONLINE/BACKUP/OFFLINE** status badge. Works on desktop and mobile without overlaying the channel name or right-side actions.
- The existing `running_seconds` / `running_started_mono_ms` time anchor, one-second local `elapsed-clock` updates, restart reset, tooltip translations (RU/EN/HR), and Offline `—` state are preserved.
- Only the web UI markup and tile CSS change. No edits to encoder selection, Intel/CPU/NVENC quality, channel start/stop, stream runtime, CBR/TS, WISI output, OSCam or service control.
- Static validation: embedded JavaScript parsed with `new Function`, exactly one uptime node appears beside the tile status badge. Actual browser responsiveness and C++ build require server-side confirmation. No service was restarted.

## V10.8.330 — NVIDIA NVENC quality A/B, without GPU-dependent default changes

- **Purpose:** test lower-macroblock H.264/HEVC NVENC at the existing selected elementary video bitrate, targeted at up to **3500 kbps SD** and about **6500 kbps HD**. The user has no NVIDIA GPU available for real A/B validation. **No claim is made that live NVENC quality is fixed yet.**
- **Safeguard:** **off by default**. Only if the operator explicitly starts a test instance with `DVBSTREAMER5_NVENC_QUALITY=1`, the shared policy adjusts NVIDIA encoder settings. Without it, the V329 P1–P7 automatic preset and **two-frame VBV remain unchanged**, including existing running services.
- **Quality A/B settings:** CPU-fed `NVENC` and NVDEC→CUDA→NVENC zero-copy use one shared `src/media/NativeNvencQualityPolicy.h`: for 720p/SD at <=30 fps choose at least P6 (quality); for larger or higher-FPS material at least P5 (balanced), preserving already selected P6/P7. VBV size rises from 2 frames to approximately **0.5 seconds** (`round(fps × 0.5)`, bounded 8–30 frames), leaving the **average and maximum encoder bitrate equal to the configured target**. This can increase scene complexity headroom without increasing the configured MPEG-TS CBR target, but can affect latency/instantaneous bursts. Use only in a controlled test.
- **Deliberately unchanged:** the proven **low-latency** NVIDIA SDK tuning, spatial AQ setting, rate-control mode CBR, GOP/IDR interval, `frameIntervalP=1`, lookahead disabled, B-frames disabled, no temporal AQ, no multipass and zero-reorder. The zero-copy path explicitly requires synchronous output; enabling lookahead/temporal AQ without managing delayed frames could regress HLS/HTTP/SRT/WISI. Presets and VBV are the only changes in the opt-in path. This is a conservative first stage, not a general switch to high-quality lookahead encoding.
- **Diagnostics:** both encoder logs include `nvenc_quality=0|1 vbv_frames=2|N requested_preset=...` and the actual `preset=pX`. Disabling the environment variable (or setting it to `0` before the next intentional process start) restores exactly the old tuning. No automatic service restart or configuration migration.
- **Independent test:** `cmake -S tests/media_core -B build-test-media && cmake --build build-test-media --parallel 2 && ctest --test-dir build-test-media -R nvenc_quality_policy --output-on-failure` verifies deterministic preset/VBV selection **without a GPU**. The full build and CPU-side media tests should also be run on Ubuntu. **Real NVENC/CUDA hardware testing and quality metrics are pending.**
- **A/B checklist on a GPU host, when available:** compare the same input/coded size/FPS/video bitrate/CBR, `nvenc_quality=0` versus `1`; check complex bright/dark scenes, `NATIVE HW ENCODER` / `NATIVE NVIDIA ZERO-COPY NVENC`, encoder speed, PCR/PTS, TS burst/CBR drops, SRT and HTTP stability. Restore `0` immediately if any input timing or decoder errors occur. Intel VAAPI V328, CPU OpenH264 V329, audio, OSCam, DVB, CBR pacer and user config not modified.

## V10.8.329 — CPU H.264 OpenH264 quality fix

- **Problem:** CPU H.264 transcoding still exhibited macroblocking on difficult dark/bright scenes; the user specifically confirmed that the V328 improvement on Intel VAAPI does **not** solve CPU. Target elementary video bitrate: SD up to 3500 kbps, HD ideally 6500 kbps, with separate MPEG-TS transport overhead.
- **CPU-only change:** replace OpenH264 2.6.0 basic `SEncParamBase/Initialize()` with the pinned upstream `GetDefaultParams/SEncParamExt/InitializeExt()` API. Enable supported Main/High **CABAC** entropy coding, adaptive quantization, scene-change detection, deblocking, rate-control overshoot correction and `bEnableFrameSkip=false` for stable live frames. Use High encoding complexity on <=720p/30fps or explicit quality/best preset and Medium on larger faster encodes to protect realtime capacity. Keep bitrate-controlled output at the user's configured video bitrate. Respect explicit H.264 Main/High profile, with Auto selecting High for coding efficiency.
- **Compatibility:** `DVBSTREAMER5_OPENH264_QUALITY=0` set in the service environment before starting the process reverts **only OpenH264 CPU** to the original basic-parameter implementation. Failed extended initialization also logs `NATIVE CPU ENCODER quality-init-fallback` and retries legacy CPU initialization rather than changing encoder type. Startup logging includes `mode=extended-quality entropy=cabac complexity=high|medium frame_skip=off`.
- **Scope:** zero changes to Intel VAAPI CABAC V328, QSV, NVIDIA NVENC/NVDEC, tuner, audio, FFmpeg decoder, PTS cadence, MPEG-TS mux/CBR, WISI Chameleon output, ticker, OSCam or service restarts. Existing running processes are unchanged until intentionally rebuilt/deployed.
- **Verification required:** build with `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --parallel 2 && ctest --test-dir build --output-on-failure`. On a **test** CPU H.264 tile, check the startup line for extended-quality and compare stills/complex scenes at 3500 kbps SD and ~6500 kbps HD, as well as CPU load, `NATIVE VIDEO ENCODE PERF`, no frames skipped, and transport stability. Also test legacy opt-out for reproducibility. **GitHub source change has not undergone local C++ compilation or runtime A/B testing.**

## V10.8.328 — Intel VAAPI H.264 compression efficiency (Main/High CABAC)

- **Observed issue:** FFmpeg input decoder + direct Intel VAAPI encoder (1024×576, 25 fps, H.264, configured/effective 2500 kbps) creates noticeable transient macroblocking on complex bright/dark scenes. The encoder was fast (~10–14 ms/frame) with no raw-frame drops. Raising video bitrate toward 4000 kbps helped only slightly; transport CBR was 5000 kbps, so this was *not* transport headroom limiting the configured bitrate.
- **Code fix:** direct VAAPI had always set `entropy_coding_mode_flag=0` (CAVLC) even when the selected H.264 hardware profile was **Main** or **High**. Now select **CABAC** for advertised Main/High capability, leaving Constrained Baseline on CAVLC. Encode-picture VAAPI flags and the manually built H.264 PPS both use the exact same flag; a mismatch would be undecodable. The encoder startup log reports `entropy=cabac|cavlc`.
- **Safe rollback:** set environment `DVBSTREAMER5_VAAPI_CABAC=0` **before starting the program** to retain V327 CAVLC output. This is a process-wide override, not a runtime toggle. Do not restart production for this test without scheduling downtime.
- **Scope:** no change to NVENC, NVIDIA zero-copy, QSV, CPU OpenH264, video decoder, transcoder FPS/PTS, mux/CBR/WISI pacing, HLS/SRT, OSCam, service configuration, defaults or user bitrate. This is a **quality A/B correction**, not a guarantee of specific picture quality: Main/High hardware driver behavior and decoded stream integrity need real-server verification.
- **Target:** evaluate 1024×576 SD at **3500 kbps** and 1080p HD near **6500 kbps** (video elementary-stream rates, not transport CBR), keeping enough transport headroom for audio and tables. Test the same material and note output H.264 bitrate, macroblocking, decoder errors and smoothness. Compare with CAVLC fallback.
- **Build/verification:** `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --parallel 2 && ctest --test-dir build --output-on-failure`; inspect `NATIVE HW ENCODER backend=vaapi-direct ... entropy=cabac`. Source change reviewed but compile and Intel-hardware playback have **not** been run here.

## V10.8.327 — consistent settings UI, interactive ticker controls and root-level Menu actions

- **Uniform settings windows:** all existing configuration modals share a blue accent for option names (including the stream editor), coherent form gaps, consistent input sizing, compact help text and visible keyboard focus. No global tile, monitoring graph or transcoder layout changes.
- **Ticker editor:** aligned font, size, text and background colors, opacity, speed and schedule controls for desktop/mobile; selected template and channel chooser remain independent. Up/down position and add/remove interval controls keep their original handlers. The speed slider now covers the same 5–600 px/s range as its numeric control, existing saved values are preserved and the preview dims when the ticker is disabled. The save button is disabled during a pending request to prevent duplicate POSTs, then restored on failure.
- **Main Menu:** move Restart program and Logout from Administration to two clearly visible actions at the bottom of the root menu. Administration keeps Configuration and About. Existing restart confirmation and logout behavior remain unchanged.
- **Scope:** changes only to the embedded web interface, version constant and documentation; media codecs, muxing, CBR, service configuration and live streams are untouched. This commit does not restart a program or tile.
- **Post-build verification:** open the editor and each settings dialog at desktop/mobile widths, test all ticker actions (channel picker/search/all/clear/edit, import, slider/number, color/opacity, position, interval enable/days/add/delete, save/cancel), and confirm that Menu root Restart/Logout still call the established handlers. A live browser/backend runtime test is still required.

## V10.8.326 — audio language/track selection without transcoding; live input/output codec display

- **Editor: input audio track.** A compact selector enumerates the actual input MPEG-TS PMT audio programs by ISO 639 language (when advertised), codec and PID, with a refresh button. `Auto` (the default for all older configurations) preserves original audio-track behavior. With *video/audio transcoding disabled*, selecting one track keeps its original compressed audio bytes and codec; it drops other audio ES PIDs and their entries from output PMT, with no software decoding/encoding. The service's video is untouched.
- **Separate input and output audio PIDs.** The new `input_audio_pid` (0 = Auto) is stored in `/etc/dvbstreamer5/dvbstreamer5-config.json`, separately from existing `audio_pid`, which remains the **output** audio PID. Output PID remap, PAT/PMT CRC/PCR and existing DVB service prefilter remain supported. Existing streams do not change unless the new option is selected. If the source stops advertising the selected audio PID, the existing strict PMT selection rejects it; choose another or Auto.
- **Actual input and output codecs.** The editing dialog title displays the channel name; a compact always-visible status row beneath it shows source and emitted video/audio codec labels, audio languages and PIDs from the **current MPEG-TS PMTs**, not merely requested transcoder settings. The API is `GET /api/media-tracks?stream_id=...`. Available while the stream is running, regardless of whether transcoding is enabled. For unrecognized/non-MPEG-TS containers or before the PMT is received, the panel shows a waiting state instead of guessing.
- **Performance:** non-DVB TS programs are inspected using an existing packet-vector PMT-only tap to avoid copying complete input packets merely to discover audio-track metadata. DVB subtitle path remains unchanged; completed output PMT is passively inspected. RU/EN/HR labels.
- **Tests:** `ctest --test-dir build -R audio_track_selection --output-on-failure` covers dual-language selection, preserving untranscoded audio, remapped output audio PID (including collision with an excluded source audio PID), CRC validity, first-track Auto legacy behavior, and language/codec discovery.
- **Deployment warning:** changing the selected track is a per-tile config change and may restart that one tile; it does **not** restart the entire DVBStreamer5 service. Validate real audio in VLC and check PMT before applying to many channels. The C++ build/tests and live IPTV/satellite output still require Ubuntu-side verification.

## V10.8.325 — PAL 25-fps source FPS gate correction (ticker video smoothness)

- **Root cause from V324 runtime traces:** the H.264 source provides ~23–26 PES pictures/s and the decoder processes all 125 pictures with ~2 ms/frame, but the old post-decoder PTS FPS gate rejects **40 of every 125** due to reordered/field-pair decoded-frame presentation timestamps. Only **85 RAW frames** reach the encoder, yielding ~16–18 fps despite 6–7 ms encode times and no queue drops. This makes ticker motion jerky even with PTS-based subpixel animation.
- **Source-verified PAL guard:** `NativeVideoSourceCadence` measures compressed H.264 picture timestamps over a rolling 33-picture window. On a verified ~23.5–26.5-fps source, only when the requested output is **25 fps** and the decoder produces **one picture per input PES**, the generic PTS-based frame *dropping* is bypassed. **No source or output PTS are rewritten** and no interpolation/duplication is performed. The previous gate is preserved for 29.97/30/50-fps sources, other target FPS, other video codecs, and multiple frames per compressed sample.
- **Live telemetry:** `NATIVE VIDEO FPS GATE SOURCE CADENCE stream_id=... source_fps=... target_fps=25 action=preserve-all-decoded-frames` confirms the bypass. After this point, `NATIVE VIDEO DECODE PERF ... fps_gate_drop_window=0` and `raw_published_fps` should approach `decoded_fps`. `use-normal-downsampler` indicates source cadence no longer qualifies.
- **Safety:** no changes to FFmpeg PAL PTS correction, NVENC/VAAPI/QSV encoder paths, audio timing, mux, TS output, HLS, SRT or OSCam. The correction affects only the diagnosed source-cadence case; monitor A/V sync after actual deployment before rolling out widely.
- **Tests:** `ctest --test-dir build -R video_source_cadence --output-on-failure` covers the exact 40/125 drop pattern, legitimate 30-to-25 conversion, 50-fps source, timestamp wrapping, missing PTS and a live rate switch. Build and test on Ubuntu before installing on production.

## V10.8.324 — compact submenus and robust ticker channel selection

- **Main Menu:** four expandable submenus with RU/EN/HR labels: **Broadcast** (VLC playlist, ticker, MPTS, XMLTV, restart schedules), **Monitoring & network** (system load, network), **Access & integrations** (user, Telegram, Newcamd, OSCam-mini), and **Administration** (configuration transfer, about, service restart, logout). Only one submenu is expanded at a time; all close on leaving the Menu. Existing actions are preserved.
- **Ticker channel picker:** use the **Select channels** dropdown with searchable checkbox rows, Select all / Clear controls and a visible selected-count badge. The adjacent **Edit** action for each channel loads its saved ticker settings into the editing form but does *not* select that channel implicitly. No channels are preselected, to prevent accidentally applying one ticker to other streams.
- **Unknown-channel save fix:** the backend now permits *unchanged, previously saved* ticker entries for deleted or renamed streams to be preserved as archived records. They are never applied to a newly added stream automatically. New references to unknown IDs or modified archived entries still fail validation. Saving settings for any currently listed channel no longer fails with `Ticker references unknown stream` just because unrelated historical entries are present. Response `archived_count` shows the count retained.
- **No streaming changes:** only UI code and ticker metadata save validation are changed. Stream video decoding, PTS clock, audio, NVENC, VAAPI, MUX, WISI CBR and OSCam remain untouched.
- **Validation:** rebuild on Ubuntu, run `ctest --test-dir build --output-on-failure`, then verify a ticker saved to one or more selected channels appears in `/etc/dvbstreamer5/ticker-overlays.json` while all unselected entries remain unchanged.

## V10.8.323 — smooth PTS-driven scrolling ticker and live speed control

- **Smoother broadcast ticker:** movement is now driven by each output stream's decoded **90 kHz video presentation timestamps** rather than wall-clock delays in the encoder queue. A frame's ticker position is independent of bursts in the encoder thread. Missing/invalid PTS uses one nominal frame step; large timestamp discontinuities are clamped to avoid jumping across a large section of the message.
- **Fractional-pixel movement:** text coverage is linearly interpolated across neighboring I420 pixels before the existing video encoder. Slow movement no longer snaps to integer horizontal positions. Other channels, untouched video encoders and transport PTS/PCR are not modified.
- **Speed controls:** Menu → Scrolling ticker now has an interactive slider for **5–300 px/s** and a fine numeric field for **5–600 px/s**. The default is **40 px/s** for new tickers; existing configured speeds remain unchanged. The miniature preview animates at the selected speed, and the panel suggests 15–50 px/s for better appearance.
- **Live changes:** saving speed or scheduling updates the overlay manager without restarting the tile. Unchanged channels preserve their independent animation phases; changing the speed of an active ticker does not teleport it back to the right edge. The new `NATIVE TICKER MOTION` log includes clock, speed and nominal FPS.
- **Regression:** `ctest --test-dir build -R ticker --output-on-failure` now checks repeated-PTS pixel stability, minimum speed and preservation of text position on a live speed edit.
- **Important:** smooth interpolation cannot create missing video frames. If `NATIVE VIDEO ENCODE PERF` still reports 16–18 fps on a supposed 25 fps channel, motion remains visually limited by that frame cadence. Diagnose the incoming/raw video frame rate separately before changing the already-working mux or GPU encoder.

## V10.8.322 — independent ticker persistence, interval pause and live on/off

- **Dedicated config file:** `/etc/dvbstreamer5/ticker-overlays.json` stores ticker entries separately from `dvbstreamer5-config.json`. The panel uses an atomic write/fsync/rename with permissions 0600; saving the ticker no longer rewrites the main stream configuration or rebuilds the UDP input firewall. Existing inline `ticker_overlays` entries from V320/V321 are migrated on first start when no dedicated file exists. A malformed independent file disables the ticker only, preserving the streaming service and source file for inspection.
- **Temporarily pause without deleting:** an *Enable scrolling ticker* checkbox controls each entry; every schedule interval independently has an *Enabled* checkbox. Disabled entries and intervals retain all text, appearance, weekday and time values. Existing V320/V321 entries default to enabled.
- **Save-to-video:** successful save immediately reconfigures the thread-safe runtime ticker manager. Toggling while a video transcoder is already using software/I420 frames affects the next decoded frame, without resetting the tile, A/V mux, TS output or audio.
- **On-demand GPU zero-copy switch:** when a scheduled ticker becomes active on a running NVDEC/CUDA/NVENC or Intel zero-copy pipeline, and the selected video decoder is not explicitly `nvidia_hw`, the video worker switches once to a frame-producing I420 decoder and its configured encoder (including NVENC if selected), keeping the tile and transport alive. There may be a short video gap while the decoder reacquires a keyframe. It stays on the I420 path for that tile's lifetime, so subsequent enable/disable changes are instantaneous. `NATIVE TICKER LIVE SWITCH` and `NATIVE TICKER SCHEDULE` logs show activation.
- **Important compatibility constraints:** video passthrough (`copy`) cannot burn text into compressed pictures. An explicitly forced NVIDIA hardware decoder (`nvidia_hw`) cannot support the CPU-frame overlay without changing its decoder configuration and restarting that individual tile; this release preserves that user's explicit decoder preference instead of silently replacing it. The live switch on real GPU hardware still requires Ubuntu-side runtime validation.
- **Regression:** `ctest --test-dir build -R ticker --output-on-failure` validates the raster and enabled/disabled intervals; compare stream PTS/PCR and FPS on the affected channel after first GPU switching. Existing channels not using ticker should retain their previous hardware path.

## V10.8.321 — ticker TXT/DOCX import, channel checkboxes and compact dark editor

- **Menu → Scrolling ticker:** select one or more output channels using a searchable checkbox grid. The separate *Channel settings template* selector loads an existing channel's settings into the editor. Save applies the displayed text, schedule and styling to all checked channels **only**, preserving every unchecked channel's previous settings. Select All / Clear work on the currently filtered list.
- **Load text from local files:** supports `.txt` (UTF-8, UTF-16 BOM and Windows-1251 fallback) and `.docx`. TXT is read in the browser. DOCX is sent only to the authenticated DVBStreamer5 server, where a bounded ZIP reader extracts `word/document.xml` with zlib and libxml2; no external conversion site, shell command or disk copy of the uploaded document is required. Paragraphs are joined into a single moving line with a separator.
- **Limits and safety:** import file ≤2 MiB, text ≤8192 UTF-8 bytes (the UI announces truncation), up to 64 ticker entries and 32 schedule windows per entry. ZIP entry size, method, bounds and CRC are validated. Empty text removes ticker configuration from checked channels only, after confirmation.
- **Appearance:** the modal uses the main DVBStreamer5 dark/blue palette, smaller inputs, a compact scrolling checkbox list, shorter preview and scrollable schedule windows. RU / EN / HR labels for new controls.
- **Backend compatibility:** the existing `ticker_overlays` persistence and FreeType burn-in pipeline remain unchanged apart from the larger text limit. Normal `/api/tickers/save` calls are limited to 1 MiB to accommodate bulk updates; `/api/tickers/import-docx` is protected by existing panel authentication.
- **Tests:** `ctest --test-dir build -R 'ticker_(smoke|docx_import)' --output-on-failure`. DOCX tests include stored and deflated ZIP entries, Unicode/Cyrillic extraction, damaged input and size rejection. Full Ubuntu compile and runtime streaming validation are required before production deployment.

## V10.8.320 — Scheduled scrolling ticker overlay

- **Menu:** renamed System to Menu (RU / EN / HR) and moved VLC playlist into the dropdown.
- **Scrolling ticker:** Menu → Scrolling ticker. Select an existing channel and enter UTF-8 text (including Cyrillic). Choose one of the fonts installed on the host (fontconfig), text/background colors, background opacity (0–100%, including transparent), speed (10–600 px/s), font size and placement using ↑ / ↓. The default is the bottom edge.
- **Schedule:** multiple independent intervals with weekday checkboxes. Add, edit or delete intervals; equal start/end represents the full selected day, and overnight intervals are supported. The server-local timezone applies. No intervals or blank text disables the overlay.
- **Persistence and API:** `ticker_overlays` in the existing `/etc/dvbstreamer5/dvbstreamer5-config.json`; panel-authenticated `GET /api/tickers` provides channels, fonts and settings, `POST /api/tickers/save` validates and saves metadata, updating the software render path live without restarting unrelated streams.
- **Rendering:** optional native FreeType glyph rasterization into I420 just before the video encoder; no change to MPEG-TS PCR, PTS, SRT, HTTP, HLS, WISI CBR or audio. Build with `libfreetype-dev fontconfig fonts-dejavu-core` to enable the overlay. Without FreeType, existing builds are supported but overlay drawing is disabled.
- **Important:** burn-in needs actual video transcoding (not H.264/HEVC/TS copy). GPU zero-copy bypasses CPU frame rendering; restarting **only the selected tile** once after first enabling a ticker selects the frame-producing decoder path. The explicit `nvidia_hw` decoder must be changed to `Auto` or `FFmpeg` for this feature; NVENC encoding can remain selected. Existing running streams without a ticker retain their zero-copy path. A save alone cannot insert graphics into an already running zero-copy encoder.
- **Regression:** `cmake --build build --target dvbstreamer5_ticker_test && ctest --test-dir build -R ticker_smoke --output-on-failure`. First compile and test on Ubuntu before deploying to production; the live hardware paths have not yet been tested.

## V10.8.295 — Intel encoder presets and QSV VBV

- **Intel preset mode** in channel editor: Auto, Balanced, Quality; persistent `intel_preset_mode` key, copied/pasted with other transcode settings. Only applies to explicitly selected Intel `qsv` or `vaapi` encoders, never NVENC/CPU.
- Intel Auto chooses Quality for 1080p+ output or low bits/pixel/second (<0.15), otherwise Balanced. Manual setting overrides that choice. Existing automatic GOP, profile, level and CBR transport headroom calculations remain.
- QSV/oneVPL applies Quality as TargetUsage=2, Balanced as TargetUsage=4. For QSV CBR use a two-second video VBV reservoir, half-full initial delay (bounded to mfxU16 fields). The effective video bitrate is used; the user's stored video bitrate remains unchanged.
- **Direct legacy VAAPI caveat:** its current H.264 implementation logs the preset but does not expose a driver quality-level or explicit VBV buffer; Intel preset selection cannot yet be guaranteed to change quality on this fallback. Do not claim the QSV VBV fix applies to direct VAAPI. This is pending hardware-specific validation.
- No changes to NVENC, audio clocks, frame order, or output TS CBR pacing. Pending Ubuntu compile, tests and comparison under the same fast, bright scene.

## V10.8.294 — Telegram configuration persistence fix

- Fix `/api/save-config` losing Telegram bot token/chat ID when a partial request omits either field, or supplies an empty value. Previously `AppConfig::fromJson` defaulted missing Telegram fields to empty, unlike preserved general/CAM settings. The in-memory client could therefore stop sending messages until credentials were re-entered.
- Stream tile editing no longer echoes cached `telegram_token`/`telegram_chat_id` values in its save payload; the server retains its existing credentials.
- The fix does not require re-entering valid credentials after deployment. It does not address Telegram Bot API network outages or invalid tokens. Avoid recording tokens in journals or screenshots.
- Requires local build/CTest and runtime verification; V294 stays separate from main until tested.

## V10.8.293 — migrate live systemd opt-ins into channel configuration

- **DVB TS jitter**: Satellite / DVB-S/S2 tile switch, stored as `jitter_ms=100` in the DVB URI; no systemd variable.
- **HTTP MPEG-TS input jitter**: channel editor switch, stored as `http_ts_input_jitter_enabled` in the normal JSON configuration. Reuses existing V283/V284 1200ms jitter reservoir with drift correction; defaults off for new channels. Remove the legacy `DVBSTREAMER5_HTTP_TS_JITTER_STREAM_ID` override only after enabling the switch on its intended channel and validating.
- **HTTP output TCP_NODELAY**: already on for public HTTP MPEG-TS since V288; no per-channel environment override needed.
- **V281 per-stream diagnostics**: `DVBSTREAMER5_DIAG_INPUT_STREAM_ID` was debug-only; not needed for normal production, should not become a global always-on setting.
- **debug.conf**: `Restart=no` and `LimitCORE=infinity` are systemd operational debugging settings, not stream processing enhancements. After regression checks, remove only `debug.conf` to restore base unit `Restart=on-failure`; do not silently embed it in the program.
- Experimental branch pending Linux compile, CTest and real DVB/HTTP playback verification. Existing active unit drop-ins are left untouched by GitHub edits.

## V10.8.292 — compare MPEG-TS production and actual client capacity

- Adds `NATIVE HTTP TS HUB PRODUCER` once every five seconds while at least one subscriber exists, with `output_kbps`, `publish_calls`, `subscriber_count`, and `max_pending_bytes`. This measures TS bytes entering the fan-out hub before TCP/socket backpressure; compare directly with V291 `acked_kbps` and `delivered_kbps`.
- A client that cannot sustain the actual TS production bitrate will eventually overflow a bounded queue. Increasing buffers only postpones failure and raises latency. This branch **does not** discard arbitrary TS packets, lower bitrate silently, or claim a TCP-only remedy.
- No changes to codecs, AV synchronization, SRT, HLS, PCI/CI, MPEG-TS timestamps, TCP transport behavior or the 4 MiB queue limit. This is a diagnostic stepping stone to a properly validated remedy (quality/bitrate match or client receive bottleneck), not an established fix.
- After building and running CTest, compare 60–120 seconds of `NATIVE HTTP TS HUB PRODUCER`, `NATIVE HTTP TS TCP STATUS`, and `NATIVE HTTP TS HUB backlog` while VLC is playing the same stream. For HLS, separate segment/manifest checks are required.

## V10.8.291 — TCP acknowledgements and receiver-window diagnosis

- Public HTTP MPEG-TS clients now log `acked_kbps`, `acked_sample_valid`, `acked_total_bytes`, `snd_wnd_bytes`, `rwnd_limited_delta_us`, and `sndbuf_limited_delta_us` alongside existing five-second HTTP TCP metrics.
- `delivered_kbps` is retained for backward compatibility but represents application writes into TCP, **not** confirmed delivery. `acked_kbps` measures change in acknowledged bytes; first sample is marked invalid.
- Extended Linux TCP_INFO is guarded by returned structure length. No changes to MPEG-TS production rate, codec/mux, HLS segmentation, SRT or socket backpressure; the 4 MiB subscriber queue cap remains.
- Build and live-test the diagnostic branch before changing the transport architecture. If production TS really exceeds network/client receive capacity, increasing queues cannot ensure both low latency and lossless delivery.

## V10.8.290 — public HTTP TCP / hub queue diagnostics

- On top of V289. Every ~5 seconds per public HTTP MPEG-TS client log `NATIVE HTTP TS TCP STATUS` with delivered_kbps, maximum blocking TCP send duration, Send-Q/notsent queue (when supported), RTT, retransmits and unacked packets. Private preview behavior unchanged.
- Log `NATIVE HTTP TS HUB backlog` once when a subscriber's pending queue grows past 2 MiB; rearm below 1 MiB. The existing 4 MiB protective disconnect remains in place. This allows correlating queue growth with TCP status without excessive logs.
- No changes to codecs, TS packet ordering, A/V mux, HLS segmentation, SRT, input jitter logic or subscriber queue limit.
- This is diagnostic, not a claimed playback fix. Build and CTest on Linux before replacing the running binary, then compare loopback curl with remote VLC during freezes.

## V10.8.288 — HTTP/HLS delivery stability trial (not yet server validated)

- Based on V287, which already has a per-tile stream uptime clock. Playback disconnects may happen without tile restarts; do not use uptime as proof that the HTTP subscriber remains attached.
- For **public HTTP MPEG-TS output**, enable TCP_NODELAY by default on each attached client socket to reduce avoidable delays in frequent 1316-byte TS writes. Private preview retains prior behavior. A nonempty DVBSTREAMER5_HTTP_MPEGTS_NODELAY_STREAM_ID limits this to the matching stream; set to `off` to disable across public outputs for A/B diagnostics.
- Extend HLS OnDemand idle grace from 10 to 30 seconds, so slower/irregular playlist polling is less likely to deactivate an otherwise watched channel. No change to TS pacing, HLS segment data or codecs.
- Keep existing V286 attach/detach reason logs and V285 per-client throughput diagnostics. These measures improve robustness but do **not** yet prove the cause of occasional VLC closure; compare client disconnect logs and inspect hub subscriber backpressure.
- Test on an isolated branch; do not merge into main without CTest and sustained HTTP+HLS playback.

## V10.8.287 — live uptime counter centered in each tile header

- Each channel's uppermost tile row now shows its own time elapsed since the streaming pipeline was successfully started. The compact `HH:MM:SS` clock (or `Nd HH:MM:SS` after a day) sits at the middle of the tile header, between the left-side stream title and existing right-side actions. The title retains a tooltip for full names; the timer has Russian, English and Croatian tooltips.
- A new immutable `StreamState::startedAt` steady-clock timestamp is set only after successful pipeline activation. `/api/state` exposes `running_seconds` and `running_started_mono_ms` per stream, zeroing both while offline; the latter identifies a genuine tile restart without confusing it with an HTTP/SRT source reconnect.
- The existing once-per-second in-browser elapsed clock updater drives tile timers without increasing the state polling rate. A fresh state snapshot only re-anchors the local timer when its startup generation changed or clock drift exceeds 2s. Stopped tiles show `—`; tile restarts immediately reset the elapsed counter when the new state is returned. The client's immediate Stop action also clears the current clock.
- Does not change the backend stream scheduler, source, transcoding, SRT/HTTP output, CA, EPG, codecs, bitrate, AV synchronization or existing stream tile group/sorting behavior. Implemented on top of the isolated V286 HTTP output diagnostic branch; no changes to `main`.
- Build/test with CTest, then confirm Start/Stop/Restart for two independent tiles and browser refresh (the clock must represent the server's true channel runtime rather than starting over on page reload).

## V10.8.286 — diagnose HTTP MPEG-TS output stalls and VLC disconnects (test branch)

- V285 is **not a confirmed solution** for the user's HTTP playback freezes and sporadic VLC closure. The supplied three-minute journal contains many `NATIVE VIDEO ENCODE PERF` entries for `stream-1793481485131` (usually 24–25 FPS, `raw_drop_total=0`, Intel encode ~15–20ms) but **no `NATIVE HTTP TS TCP_NODELAY`, `NATIVE HTTP TS OUTPUT PERF`, or `subscriber backlog limit`**. This does *not* establish that the observed VLC connection uses this server's HTTP MPEG-TS output path or that the V285 environment override is active.
- The HTTP public endpoint routes through `HttpServer::handleHttpStream` -> `StreamManager::addHttpClient` -> `NativePreviewHub` local `SOCK_SEQPACKET` -> per-client blocking TCP writer. The client or local transport may close without recording a specific cause. Such disconnects are independent of the ongoing encoder and might explain the VLC behavior.
- **Diagnostic only**: log every HTTP MPEG-TS client `NATIVE HTTP TS CLIENT attach` with stream ID, socket FD, hub FD, public/preview kind and effective TCP_NODELAY; `NATIVE HTTP TS CLIENT detach` with `reason=hub_eof|hub_read_error|tcp_send_error`, errno, session duration, bytes and chunks successfully sent. Log `rejected` for missing stream/hub or subscribe errors, and `NATIVE HTTP TS HUB detach` for a local subscriber socket failure or pending-queue saturation. Do not include stream URLs, authorization values or client IPs in diagnostic logs.
- The existing V285 `NATIVE HTTP TS OUTPUT PERF` is still opt-in via `DVBSTREAMER5_DIAG_INPUT_STREAM_ID`. TCP_NODELAY remains opt-in via `DVBSTREAMER5_HTTP_MPEGTS_NODELAY_STREAM_ID`. The actual TS byte path, 2MiB HTTP input jitter queue, native transcoder, SRT, CBR, PCR/PTS/DTS, socket timeouts and client buffer sizes remain unchanged.
- To check whether the tested HTTP URL reaches this handler, make a controlled loopback request on the configured HTTP port (for example `/stream/stream-1793481485131.ts`), then inspect `NATIVE HTTP TS CLIENT attach/detach`. If loopback logs appear but the failing VLC playback does not, verify which server, port, reverse proxy or legacy URL the player actually uses before changing transport behavior.
- V285's one-shot small TCP-send improvement is a hypothesis; do not merge either branch into main without reproducing/fixing actual VLC disconnect reason. Build and run all CTest cases and compare exact HTTP endpoint versus SRT playback.

## V10.8.285 — targeted HTTP MPEG-TS TCP low-latency output experiment

- On `svettranscoding`, source `stream-1793481485131` (Любимое кино) is **HTTP MPEG-TS input**, not DVB. V284's opt-in input jitter prebuffer remains stable and CTest is 5/5; recent log windows have 24–25 fps, zero RAW drops and zero AV mux late/drop counters. The user reports visible pauses on **HTTP MPEG-TS output** while receiving the same stream smoothly via **SRT output**, pointing to output-specific delivery rather than the input/codec path.
- HTTP output currently uses a production `NativePreviewHub` SOCK_SEQPACKET fan-out (typically 1316-byte TS-aligned datagrams) and one TCP socket writer per HTTP client. Unlike SRT UDP transport, these small TCP writes were not configured with `TCP_NODELAY`, so Nagle/delayed ACK interaction may add HTTP-client delivery jitter. This is a hypothesis to test, not a proven source of stalls.
- **Off by default.** For a single matching public HTTP MPEG-TS channel, set `DVBSTREAMER5_HTTP_MPEGTS_NODELAY_STREAM_ID=stream-1793481485131` in systemd. On HTTP client connection, set `TCP_NODELAY=1` on the client's TCP socket, not on its input URL nor on SRT. A failure to set TCP_NODELAY is logged but does not disconnect an otherwise usable client. Log `NATIVE HTTP TS TCP_NODELAY stream_id=... enabled=1` on success.
- If `DVBSTREAMER5_DIAG_INPUT_STREAM_ID=stream-1793481485131` is also set, emit `NATIVE HTTP TS OUTPUT PERF` every 1024 TS socket reads: `read_gap_max_ms`, `read_gap_over_80ms`, `tcp_send_max_us`, `tcp_send_over_20ms`, and `tcp_nodelay`. The diagnostic runs only for the matching HTTP client, outside the fan-out producer lock. Compare with V284 `NATIVE VIDEO INPUT CADENCE`, `NATIVE HTTP JITTER STATUS`, SRT output playback, and visible freezes; independent 15-second `curl --no-buffer` sampling on the HTTP output can distinguish receive timing.
- No change to codec, HTTP input jitter buffer/feedback, MPEG-TS headers/PCR/PTS/DTS, CBR pacing, CA, subtitle/EPG, audio mux, SRT, UDP or HLS. Browser-private preview sockets are deliberately excluded. Do not merge into main before server CTest and real client comparison with the environment option toggled.

## V10.8.284 — HTTP jitter reservoir drift correction (experimental)

- V283 live testing on `svettranscoding` channel `stream-1793481485131` showed 24–25 FPS, `raw_drop_total=0`, `raw_vq_peak=1–3`, no AV mux video/audio drops or late events, and Intel encoding usually 15–18ms per frame. Thus the 1.2s prebuffer/paced-input design successfully eliminates the ~460ms TS feeding gaps for this channel.
- A remaining long-run issue in V283: `NATIVE HTTP JITTER STATUS` showed the input backlog rising from roughly 400–540KiB toward 750–806KiB despite an approximately 520–537KiB target. V283 applied only 10% correction after crossing half/1.5x target, leaving a wide zero-correction region and allowing growing live latency.
- Replace the two-threshold controller with continuous proportional reservoir feedback `correction_pct = clamp(40 * (backlog_bytes - target_bytes) / target_bytes, -15, +30)`. Effective byte-pacing bitrate is measured HTTP bitrate times `(100 + correction_pct)/100`. A full queue is drained gradually faster, a low queue is consumed more slowly; elapsed-wall-clock pacing continues to reanchor rather than sending a catch-up burst. Clamp target to 16KiB..1.2MiB so it is achievable with the existing 2MiB bounded receive queue.
- Add `correction_pct` and `output_kbps` to `NATIVE HTTP JITTER STATUS` for diagnosing steady-state buffer drift. A regression check of the neutral, positive, negative, saturated and zero-target cases is added to the existing `native_protocols` CTest.
- The `DVBSTREAMER5_HTTP_TS_JITTER_STREAM_ID` opt-in remains required and only acts on the exact HTTP MPEG-TS channel. No changes to DVB, UDP, SRT, remap, CA, TS packets, PCR, PTS, DTS, CBR output, audio, Intel/NVIDIA encode, FPS gate, or AV mux scheduling. Keep the main branch unchanged; build + CTest and live comparison required before considering merge.

## V10.8.283 — opt-in HTTP MPEG-TS jitter smoothing for one stream (experimental)

- Independent `curl --http1.1 --no-buffer` pipe read on `svettranscoding`, bypassing DVBStreamer5 entirely, captured 6.26 MiB in 15s with 480.9ms maximum receive gap, 31 intervals above 200ms, and 1017 of 1050 intervals below 5ms. V282 directly observed the same 451–467ms HTTP callback gaps, while relay enqueue waited only 0–2ms and the relay queue peaked below 100KiB. Source/delivery batching — not decoder or Intel GPU saturation — causes RAW bursts.
- **Disabled by default**. To enable on precisely one live HTTP MPEG-TS channel, set systemd environment variable `DVBSTREAMER5_HTTP_TS_JITTER_STREAM_ID=stream-1793481485131` (and restart). Its matching stream must use direct `http://` or `https://` input and cannot be externally fed. Existing `DVBSTREAMER5_DIAG_INPUT_STREAM_ID` continues to enable only diagnostics.
- Once opted in, the receiver keeps enqueuing into the existing bounded 2MiB ingress queue; producer chunk size is limited to 8192 bytes for finer downstream timing. The relay holds about 1200ms for a reservoir, estimates the input rate from received bytes, and releases one chunk at an interval proportional to chunk bytes / measured bitrate. Rate is gently updated every 3s to follow changing input bitrates, with a +/-10% correction based on queue occupancy to prevent sustained queue drift.
- Input packets remain unchanged: **PCR, PTS, DTS, payload bytes, existing stream codecs, video FPS gate, Intel/NVENC options and AV mux synchronization are untouched**. This intentionally increases the targeted live channel latency by about 1.2s, and cannot restore packets missing at the source. It aims to avoid 460ms empty input gaps and bursts of decoded RAW frames that can exceed the existing 12-frame raw queue.
- Logs `NATIVE HTTP JITTER enabled`, `NATIVE HTTP JITTER ready` and periodic `NATIVE HTTP JITTER STATUS` with bitrate and queue occupancy. On HTTP reconnect, reset the pacing anchor and start a fresh reservoir. Explicit opt-out: remove the env variable/drop-in and restart; no persisted config migration is needed.
- **Not yet compiled on the server or live validated**. Run CTest 5/5 first; then compare `NATIVE VIDEO INPUT CADENCE`, `NATIVE VIDEO ENCODE PERF`, `NATIVE HTTP JITTER STATUS` before/after under the same source. Never merge into main without live proof.

## V10.8.282 — native HTTP MPEG-TS input/relay cadence diagnostics (test branch)

- The actual freeze test stream `stream-1793481485131` (Любимое кино) uses **`http-ts`** input on `svettranscoding`, which has **no DVB tuners**. V281 shared-DVB diagnostics are intentionally inactive for HTTP inputs; do not infer anything about tuner or LinuxDvbInput from missing V281 messages.
- V280's live results show 10–12 approximately 460ms gaps in each 125-PES window, with most PES received in <5ms bursts, and nearly identical TS/PES gaps. Software decode is about 3–4ms/PES; RAW scaling and Intel encoding are fast enough for 25fps in normal windows. The outstanding issue is uneven delivery *before* decoding, but source/network vs internal queue/relay backpressure remains unproven.
- Add a diagnostic stream identity to the native relay (set by StreamManager, does not modify transport headers). With `DVBSTREAMER5_DIAG_INPUT_STREAM_ID=stream-1793481485131`, an HTTP-only `NATIVE HTTP SOURCE CADENCE` line appears every 125 network receive callbacks: `network_gap_max_ms`, `network_gaps_over_200ms`, `network_burst_under_5ms`, `enqueue_max_ms`, `enqueue_over_50ms`, bytes and chunk count. Enqueue timings include optional waiting for the existing 2MiB queue.
- A matching `NATIVE HTTP RELAY CADENCE` line appears every 125 HTTP input chunks consumed by the relay: `relay_gap_max_ms`, `relay_gaps_over_200ms`, `transform_max_ms`, `loop_max_ms`, `loops_over_100ms`, `queue_peak_bytes`, bytes and chunk count. `loop_max_ms` includes downstream packet handling/output, not just transcoder process().
- Interpretation: sustained `network_gap_max_ms` about 450ms with small enqueue/transform/loop times suggests server-side batching or HTTP receive/network delays; short network gaps with large `enqueue_max_ms` or `queue_peak_bytes` approaching 2097152 suggests queue backpressure; short source gaps but large `loop_max_ms` or `relay_gap_max_ms` suggests processing/output stalls. The windows are independently grouped by 125 chunks and not time-aligned to the 125-PES decoder metrics.
- Diagnostics are opt-in and do not change buffer sizes, HTTP timeouts, CBR output, MPEG-TS contents, decoder selection, frame pacing, audio/video PTS, Intel/NVIDIA transcoding or mux policy. Run full CTest and live measurements before a behavioral fix or main merge.

## V10.8.281 — shared DVB source and subscriber cadence tracing (experimental)

- V280 CTest passed all five tests. Live channel `stream-1793481485131` repeatedly exhibits 445–479 ms gaps at MPEG-TS ingestion matched by ~454–480 ms gaps at ready video PES (10–11 such gaps per 125 PES), followed by massive <5 ms PES bursts. Native decoding is ~3–4 ms/PES, queue drops are near zero after startup, and the Intel encoder is not the cause of these long upstream stalls.
- The shared DVB source has a dedicated transport reader, per-service subscriber queues, a service prefilter, then the native relay input queue. To pinpoint the earliest pause, the shared DVB reader reports `SHARED DVB SOURCE CADENCE frontend=... read_gap_max_ms read_gaps_over_200ms poll_max_ms poll_empty` every 125 *successful* DVR reads only when the env-selected stream is subscribed to that frontend. Poll timeouts are counted but never treated as an error by this diagnostic.
- Opt-in per-service `SHARED DVB SUBSCRIBER CADENCE stream_id=...` measures gaps between callback dispatches, callback wall time, callback delays >100 ms, and queue backlog peak. Only the service selected by the environment variable `DVBSTREAMER5_DIAG_INPUT_STREAM_ID` is logged, to prevent excess diagnostic logging across shared transponders.
- Compare the two new messages to V280 `NATIVE VIDEO INPUT CADENCE`: source-reader gaps identify frontend/kernel feed stalls, subscriber-only gaps suggest scheduler/prefilter delays, and healthy source/subscriber with long transcoder input gaps implicates the relay input queue or downstream transform loop. Pauses are observations, not necessarily lost source packets; inspect packet continuity and playback before assuming a tuner fault.
- No source timing correction, new buffering, CA algorithm, PCR/PTS changes, queue reconfiguration, decoder, mux or video encoder behavior. Run CTest 5/5 and a controlled live check before deployment/merge.

## V10.8.280 — PTS diagnostic unsigned integer compile fix

- Fixes GCC 13 compilation of the V279 input video PTS telemetry: `std::max()` requires matching argument types. Explicitly makes the computed PTS gap `std::uint64_t` instead of deducing `unsigned long long` from `/ 90ULL` on a system where `std::uint64_t` is `unsigned long`.
- All V279 TS/PES cadence diagnostics remain enabled. No decoder, mux, scaling, PTS value, FPS gate, audio, queue or transport behavior changed. Server build/CTest not yet verified.

## V10.8.279 — TS ingress versus demux PES arrival cadence (diagnostic branch)

- On working stream `stream-1793481485131`, V278 reports decode `~2ms/PES`, stable windows with `125 decoded/125 PES` and no PES queue or FPS gate drops. But the decode worker waits up to 446–569ms between input PES and the encoder occasionally overflows a 12-frame RAW queue during catch-up bursts, even though Intel VAAPI encode itself is fast enough.
- Adds `NATIVE VIDEO INPUT CADENCE` every 125 video PES: `pes_arrival_gap_max_ms`, `pes_gap_over_80ms`, `pes_gap_over_200ms`, `pes_burst_under_5ms`, `pes_pts_gap_max_ms`, `pes_pts_gap_over_80ms`, `ts_input_calls`, `ts_input_bytes`, `ts_input_gap_max_ms`, `ts_gap_over_80ms`, `ts_gap_over_200ms`.
- Compares wall-clock gaps at `NativeTranscoderPipeline::process()` ingress against arrival gaps of completed video PES from `NativeTsDemux`. Long TS ingress gaps suggest the upstream input path or caller batching; continuous TS ingress with large PES gaps suggests PES assembly, missing/late video packets, or source timebase irregularity. PTS changes distinguish a media timestamp jump from transport timing gaps; they cannot by themselves prove the input transport is at fault.
- All counters are per-pipeline and updated under the existing process mutex; compressed PES and TS payloads, mux PTS, deinterlacing, codec choice, FPS gate, queue capacities, drop policy and transport output are unchanged.
- The first interval after a restart can be atypical. Compare several consecutive `INPUT CADENCE`, `DECODE PERF` and `ENCODE PERF` windows for the same stream. Test 5/5 before deployment; do not merge to main before live validation.

## V10.8.278 — per-stage source PES / AVC decode / frame-gate diagnostics (experimental)

- V277 showed the hardware encoder spends only ~21ms processing a 1080p frame (5ms CPU upscale + 15ms Intel encode), but observed RAW-frame arrival was ~22fps and the encoder worker spent ~3068ms of its 125-frame window idle, including one 830ms gap, with zero RAW queue drops in that measurement.
- New per-stream `NATIVE VIDEO DECODE PERF` emitted every 125 processed video PES: `pes_in_fps`, `pes_processed_fps`, `decoded_fps`, `raw_published_fps`, `decoded_frames`, `raw_published_frames`, `fps_gate_drop_window`, `decode_avg_ms`, `decode_max_ms`, `decode_empty_wait_ms`, `decode_empty_wait_max_ms`, `decode_empty_wait_events`, `pes_queue_now`, `pes_queue_peak` and `pes_queue_drop_total`.
- PES input counts are atomically owned by the demux producer; decoded/RAW-published counts are updated only on the video decoder worker. The snapshot of compressed-PES queue/drop counters is taken under its existing mutex. No extra per-frame log lines or changes to decoding, video frame order, FPS gate, audio timing, hardware acceleration or backpressure policies.
- Cross-check `NATIVE VIDEO DECODE PERF` and V277 `NATIVE VIDEO ENCODE PERF` for the **same stream_id after startup**. Input PES stalls vs. slow decoder vs. frame-gate drops can now be distinguished. Both logs aggregate over 125 samples but are not guaranteed to be exactly time-aligned.
- This is a diagnostic test branch, not proof of a throughput fix. Run CTest 5/5 and real streaming before main merge.

## V10.8.277 — RAW producer cadence versus encoding throughput diagnostics (test branch)

- V276 full CTest passed 5/5 and a production observation found CPU bilinear scaling as low as 5ms/frame (versus V275 16–17ms), while the software scaler output remains byte-identical in the regression test. Despite a combined ~21–30ms scale+encoder time, the 1920x1080 live pipeline still intermittently achieves only ~21–24fps and accumulates pre-encoder RAW queue drops.
- Added per-stream, 125-frame `NATIVE VIDEO ENCODE PERF` fields: `raw_in_fps` (estimated frames enqueued by decoder), `raw_in_frames`, `raw_drop_window` (pre-encoder RAW frames dropped in this interval), `raw_empty_wait_ms` (total time the encoder worker observed an empty RAW queue before dequeuing), `raw_empty_wait_max_ms`, and `raw_empty_wait_events`.
- `raw_in_frames` uses queue conservation: 125 processed frames + drop delta + ending queue depth - starting queue depth. Estimates vary slightly across telemetry boundaries due to concurrent producers, but isolate bursts/stalls without extra per-frame producer logging or synchronization.
- **No changes to frame dropping, queue capacities, live A/V synchronization, FPS gate, codec settings, CPU scaling mathematics, Intel/NVENC paths, and HTTP/SRT transport.** This branch is instrumentation only and needs CTest and real 25fps streaming logs before a permanent scheduler or queue fix.

## V10.8.276 — exact CPU I420 upscaling performance optimization (test branch)

- A profiling run on the actual Intel transcoder (`stream-1793481485131`, 1920x1080) showed 16–17 ms per frame spent in CPU I420 scaling versus 15–16 ms in VAAPI H.264 encode/mux submission. The 12-frame raw-video queue repeatedly filled and accumulated hundreds of pre-encoder drops even when AV mux late/drop counters remained zero.
- Optimizes the native bilinear I420 scaler with *separable fixed-point* interpolation, caching two horizontally interpolated source rows while output rows advance. The interpolation weights, 8-bit precision, 32768 rounding and >>16 final conversion match the original calculation exactly; no quality downgrade is intended.
- Keeps **separate thread-local luma and chroma geometry maps**, avoiding the former Y->U->V->Y map rebuild every frame. Cached horizontal row values are invalidated on each plane call, so no pixels are reused across frames or chroma planes.
- Adds a six-geometry regression in `native_transcoder` that compares the optimized output **byte-for-byte against the former four-tap formula** (including 720x576->1920x1080, downscale, identity, frame-to-frame changes), and checks preserved frame PTS/key metadata. The test stays active alongside native H264 encode/decode tests.
- No changes to Intel/NVENC selection, CBR, frame pacing, timestamping, MP2, SRT/HTTP transport, queue lengths, or existing V275 live behavior. Build + 5/5 CTest + live FPS/raw-drop check required before production or main merge.

## V10.8.275 — HLS duration and encrypted CMAF regression tests (experimental)

- Fixes the synthetic HLS output test configuration: the `NativeHlsSegmenter` defaults to a 4-second segment target unless `durationExplicit=true`. The test's requested 1-second target is now explicit; no production segmenter behavior has changed.
- Updates the CBCS encrypted CMAF fixture to include sufficiently long video and AAC access units with complete 16-byte protected blocks. Preserves the AES-CBCS encryption and decryption code unchanged.
- CBCS test scans fragments in stable filename order, decrypts and remuxes them all, and **requires at least one fragment to change**; short/no-encrypted-block fragments alone do not cause false failures. On failure, reports fragment count, largest fragment, source TS bytes and output TS bytes.
- Retains V274 tested automatic FFmpeg recovery after silent Ittiam AVC startup. All five CTest cases and live verification are required before deployment or main merge.

## V10.8.274 — silent Ittiam AVC startup recovery / HLS and CBCS test fixtures (experimental)

- When the explicit Ittiam AVC decoder consumes six initial access units without a single output frame, it now attempts a **one-time**, bounded (max 4 MiB) replay through the optional FFmpeg AVC decoder. The switch happens only if FFmpeg actually decodes pictures. Normal Ittiam output is not decoded twice, and VAAPI/NVENC/mux/PTS scheduling settings remain unchanged.
- Corrects synthetic HLS rotation test packets to begin with a video PES header and sequence header; previous PCR-only packets were rightly rejected by the clean-PES-start safeguard, resulting in zero segments even in independent mode.
- CMAF SAMPLE-AES/CBCS protocol test now uses a separate fixture with longer H.264 video access units so the CBCS encrypted subsample region contains full 16-byte AES blocks. CBCS correctness assertion remains enabled; the production encryption and decryption algorithms are unchanged.
- Inherits V272 end-of-input mux drain behavior and previous HTTP/SRT/MP2 improvements. Requires compilation and full 5-test CTest plus live broadcast validation before deployment or merge into main.

## V10.8.273 — HLS fixture update and codec/CMAF test diagnostics (experimental)

- HLS output CTest uses the independent-segment path for its deliberately synthetic PCR/random-access-only packet stream. Unlike the normal passthrough HLS path, this scenario does not require PAT/PMT and valid elementary PES. Reports actual segment count, target duration and endlist flags on failure.
- CMAF CBCS CTest now prints fragment size and presence of moof/traf/senc/mdat metadata if decryption reports success without altering data. This remains a failing test until the reason is confirmed and resolved; do not disable CBCS correctness checks.
- Native H.264 transcoder CTest now reports actual output TS size, video PES count, decoder errors, and comparison frame counts from the explicit FFmpeg AVC decoder versus the factory decoder (Ittiam). It retains the decode success assertion and will not falsely pass on empty output.
- Retains the V272 end-of-input AV mux drain fix and V267–271 live pipeline work. No production codec, mux, transport or synchronization logic was changed in this V273 branch. Server compile and tests pending, not for main.

## V10.8.272 — finite-input mux drain fix (test branch)

- Fixes the video-only `native_transcoder` CTest failure `flush: native transcoder async queues did not drain`: the live mux initially waits up to 2.5 s for a missing audio peer, while `waitForIdle()` times out after about 1 s. Explicit `flush()` now temporarily bypasses live peer-wait and A/V lead holds to drain a complete, finite input, then restores the live mux policy on all return paths.
- Streaming A/V synchronization limits, FPS, bitrates, queues, Intel/NVENC selection and existing SRT/HTTP work remain unchanged.
- Adds failure diagnostics for transcoder flush queue status, HLS fixture request/byte counts and the first failing RTSP/RTMP/CMAF protocol stage. HLS and protocol failures still require server-side CTest output to diagnose; not claimed fixed.
- This branch has not been compiled or tested on the user's server. Do not merge into main without the 5-test suite and live validation.

## V10.8.271 — split software scaling and Intel encoding timing (test branch)

- For each 125-frame video encode interval, `NATIVE VIDEO ENCODE PERF` additionally reports `scale_avg_ms`, `scale_max_ms`, `encode_avg_ms`, `encode_max_ms`, attributed per stream ID.
- Includes CPU I420 scaling (for 720x576 -> 1920x1080) separately from hardware encoding + mux submission. No modification to the video processing results, 25-FPS gate, queue depth, drop policy, bitrate or output timestamps.
- Allows diagnosis of frame drops on a functioning stream without relying on a temporarily offline input source. Inherits V270 accurate SRT peer/queue metrics and V267–269 fixes.
- Experimental: build, unit tests and live HTTP/SRT playback testing are required before any main merge.

## V10.8.270 — SRT loss attribution and peer health (test branch)

- Corrects misleading `NATIVE SRT TS queue overrun` messages: the former threshold used `droppedBytes`, which also increased when a listener had no connected SRT peer. The new warning reports **actual producer queue overflow only**.
- Separately counts true SRT producer-queue overrun bytes and expected bytes discarded with no listener peer; retains the old aggregate dropped-byte count for compatibility.
- Adds per-SRT-output `NATIVE SRT OUTPUT HEALTH` telemetry every 10 seconds: stream ID, connected state, queued/sent bytes, actual queue overflow, no-peer discard, total drops and reconnects.
- Regression test covers a listening SRT output with no peer (offline drops are not queue overflow).
- No changes to scaling, Intel VAAPI settings, 25 FPS cadence, mux timestamp logic, transport packet size or SRT latency. Existing V267–269 work retained. Build and live tests required before main merge.

## V10.8.269 — video encode / raw queue diagnostic test branch

- Tags all `NATIVE PRIMARY RAW DROP` and `NATIVE ABR RAW DROP` messages with the stream ID.
- Adds one `NATIVE VIDEO ENCODE PERF` diagnostic per 125 encoded frames (about 5 s at 25 fps) with effective throughput, average/peak scale+encode time, current/peak raw queue depth and total primary drops.
- No queue-size, timestamp, codec-selection or transport changes: diagnose whether HTTP MPEG-TS and SRT freezes originate before both outputs.
- Inherits V268 HTTP/SRT TS integrity fixes and V267 MP2 cursor repair. Build and live tests pending; keep outside main.

## V10.8.268 — HTTP MPEG-TS / SRT packet integrity (test branch)

- HTTP production MPEG-TS now retains TS-aligned packets in an ordered, bounded per-subscriber backlog when the nonblocking socket reports EAGAIN. It no longer silently discards the remainder of each transport batch. Clients that fall over 4 MiB behind are disconnected rather than receiving silently corrupted TS. Increased HTTP socket send buffer to absorb short bursts.
- SRT output's queued chunks are TS packet-aligned (multiples of 188 bytes), preventing partial-packet boundaries when dropping old queue data under congestion. Emits a throttled backlog-overflow diagnostic.
- Added an HTTP backpressure regression test that verifies 4,096 TS packets arrive in order after the subscriber has temporarily stopped reading.
- Inherits V267 MP2 cursor repair. This branch still needs server compilation and end-to-end HTTP/SRT playback tests before main merge.

## V10.8.267 — PL_MPEG MP2 cursor underflow fix (test branch)

- Normalizes the PL_MPEG bit cursor before reading its remaining byte count or appending input, preventing the unsigned underflow reported as `pending=18446744073709551615` and the resulting unsafe `memmove`.
- Repairs small bit-cursor overruns immediately after an MP2 decode attempt, avoiding needless decoder resets and audio interruptions. Invalid large cursor overruns still reset safely.
- Retains V266's bounded buffer, and leaves FFmpeg/Intel/NVENC video paths untouched. Server-side build, playback and multi-stream restart tests are required before merging.

## V10.8.266 — MP2 decoder buffer safety (test branch)

- Limits individual MPEG Layer II audio input packets and unread PL_MPEG streaming-buffer backlog.
- Recreates the MP2 decoder after a stalled backlog, and guards decoded sample count and failed writes.
- Intended to address the SIGSEGV in `plm_buffer_write` seen when two IPTV tiles were active. Root cause still requires runtime validation; no changes to H.264 decoder choice or Intel VAAPI encoding.

### V10.8.265 — OpenH264 removed from H.264 decoder selection

- Removed the OpenH264 **video decoder** class and factory, the manual dropdown choice, and the Auto video-decoder fallback. Legacy saved `openh264` selections are remapped to `auto` at runtime and shown as Auto in the editor.
- The OpenH264 **CPU video encoder** remains available; do not remove codec libraries or the existing CPU H.264 encoding path.
- Auto software decoding now tries FFmpeg then Ittiam (after compatible hardware decoding); manually selected Ittiam and FFmpeg retain strict selection.
- The 150-ms software AVC mux envelope from experimental V10.8.263 remains in this branch. Build and regression-test before merging to main.

### V10.8.264 — OpenH264 Annex B recovery (experimental)

- Stop feeding SPS/PPS via a separate failing `DecodeFrame2` prime before IDR. Supply cached Annex B SPS/PPS together with the IDR access unit when missing from that unit.
- When OpenH264 has not produced any video, log bounded sync-pending diagnostics; explicit OpenH264 remains strict, with no fallback to other decoders.
- This is a hypothesis-driven recovery for repeated `dsNoParamSets`/`dsBitstreamError`; an unsupported interlaced stream may still produce zero frames. Live test with `NATIVE AVC SAMPLE DIAG` and `NATIVE AVC OUTPUT` before merging into main.

### V10.8.263 — экспериментальный AV-mux hard lead 150 мс для software H.264

- Для вручную выбранных Ittiam/OpenH264 и программного AVC в Auto включена такая же ограничивающая политика 150 мс, какая ранее работала для FFmpeg. Цель: устранить подтверждённые ~480 мс опережения видео и непрерывный рост late_audio на Ittiam.
- Не изменены исходные PTS, PAL cadence, PCR, аудио, hardware zero-copy или fallback ручных декодеров. GPU zero-copy сохраняет 500 мс.
- Это *тестовый* патч: на одном канале обязательно проверить зависания/недостаток пакетов, mux queue, late_audio и воспринимаемую синхронизацию; при ухудшении откатиться на V262. В main не сливать до проверки.

### V10.8.262 — строгий ручной выбор AVC-декодера и аппаратный Auto

- Ручной выбор OpenH264, Ittiam или FFmpeg не переключается на другой программный декодер при ошибке, паузе декодирования или H.264 field coding; вместо этого ошибка остаётся видимой в журнале.
- Фабрика OpenH264 больше не подменяет выбранный OpenH264 на Ittiam скрытым диагностическим fallback.
- AVC Auto: существующие NVIDIA/Intel zero-copy проверки выполняются раньше; если совместимый аппаратный zero-copy путь не запущен, пробуется независимый Intel HW decoder, затем FFmpeg → Ittiam → OpenH264.
- Для выбранных вручную NVIDIA/Intel аппаратных декодеров не предусмотрена скрытая замена на программный.
- PCR, общий mux, audio timing, видеоэнкодеры и FFmpeg PAL PTS не изменены. Проверить полную сборку и live-регрессию перед слиянием в main.

### V10.8.261 — программные H.264 декодеры и переход PTS через 33-битную границу

- **База V260 сохранена:** успешная реальная синхронизация аудио/видео в VLC, PCR decode preroll 500 мс, аудио PTS, VAAPI/QSV и NVIDIA NVENC/NVDEC не менялись; ручного аудио-смещения нет.
- **Ittiam libavc:** прежний wrapper без проверки присваивал каждому interlaced output 25-fps PTS от первого кадра, даже при другой реальной частоте или обрыве GOP. Теперь `NativeIttiamAvc.cpp` использует проверяемый `FfmpegPalPtsClock` с обнаружением только устойчивой PAL 25-fps cadence и повторной привязкой при discontinuity, без изменения самой сборки libavc.
- **OpenH264 и Ittiam при interlaced broadcast AVC:** `NativeTsDemux` передаёт по каждому AU признак фактически распознанного `field_pic_flag`. Если он появился у программного OpenH264/Ittiam, только этот поток переключается на проверенный FFmpeg/libavcodec, поскольку OpenH264 не гарантирует корректное декодирование H.264 field-coded picture. Для прогрессивного H264 сохраняется выбранный декодер. Если FFmpeg не собран, выбранный декодер остаётся активным, а причина отказа записывается в журнал.
- **Автовосстановление:** если программный AVC декодер возвращает ошибку или 150 входных AU подряд не выдаёт кадры, попытаться один раз переключиться на FFmpeg без изменения видеоэнкодера, звука или других каналов. Логи: `NATIVE AVC SOFTWARE FALLBACK stream_id=... from=... to=ffmpeg reason=verified_h264_field_coding|no_decoded_frames|decoder_error`. При неуспехе журнала `unavailable` исходный тракт сохраняется для случаев без фатальной ошибки.
- **Потоки и очереди:** при динамическом выборе FFmpeg сигнал в независимый mux worker передаётся через атомарный `ffmpegAvcMuxPolicy_` (без гонки при чтении `selectedAvcDecoder_`). Действующие ограничения 150/500 мс сохраняются.
- **33-bit PTS/DTS wrap:** выявлен затык `muxAudioQueue_=512`, `drop_audio>47000`, `av_delta_ms~-94500000` когда видео прошло `2^33` 90-kHz ticks, а аудио перешло к нулю. Общие сравнения, выбор пакетов, late/drop и журнал используют модульные функции `src/media/PtsClock33.h`; `NativeMpegTsMux` продолжает отсчёт внутреннего времени сквозь wrap и сохраняет разницу B-frame PTS−DTS. Дополнительный header-клон удалён.
- **Регрессия:** `testPtsClock33()` добавлен в `tests/test_media_core.cpp`. Проверяет события непосредственно до/после wrap, совместимость wrapped/unwrapped меток и регресс `video=2^33+60000, audio=55000`. Небольшой отдельный C++17 smoke test логики clock прошёл локально; полный `media_core_tests`, программные декодеры и live GPU/OSCam после V261 **должны быть проверены на сервере**.
- Команды проверки: `cmake -S tests/media_core -B build-test-media && cmake --build build-test-media --parallel 2 && ctest --test-dir build-test-media --output-on-failure`. Сначала тестировать V261 на одном канале с FFT/FFmpeg, затем Ittiam/OpenH264; сохранять рабочий V260 для отката. Метрики: `NATIVE AVC SOFTWARE FALLBACK`, `NATIVE AVC ITTIAM PAL cadence`, `NATIVE AV MUX`, `NATIVE AV MUX DROP`.

### V10.8.260 — исправлены тесты HTTP live reconnect (media_core)

- После прохождения CBR/UDP/RTP частей тестов V259 `media_core_tests` упал в `testNativeHttpCbrRelay(): assert(!relay.isRunning())`. По серверному журналу `NATIVE HTTP INPUT ... status=200 ok=1 bytes=1504` и следующая `attempt=2`: исходная функция `NativeUdpRelay::runHttpInput()` намеренно повторяет подключение после EOF обычного конечного HTTP ответа; live relay НЕ обязан завершаться сам.
- Исправлены тесты HTTP CBR, HTTP query access key и HTTP VBR/RTP fanout: после получения полезных пакетов проверяем, что соединение/live relay остаётся работающим, затем явно выполняем `relay.stop()` и ожидаем корректное завершение одноразового тестового HTTP-сервера. Устаревшая проверка пустого `lastError()` после transient EOF удалена, так как состояние `reconnecting` является нормальным.
- Проверки HTTP 404 и HTTP 302 с авторизационным ключом теперь ожидают соответствующие сообщения ошибки **при работающем relay**, затем `stop()`. Продолжаем проверять, что секретный заголовок присутствовал только в первоначальном запросе и перенаправление не было выполнено. Никакие дополнительные подключения от имени тестового пользователя не выполняются.
- Изменён исключительно `tests/test_media_core.cpp`; production `NativeUdpRelay.cpp`, WISI CBR pacing, remapper, mux, видеодекодеры и выходные протоколы **не изменялись**. PCR preroll 500 мс из V256 оставлен без изменений.
- Запуск в Ubuntu: `git pull --ff-only origin main && cmake -S tests/media_core -B build-test-media && cmake --build build-test-media --parallel 2 && ctest --test-dir build-test-media --output-on-failure`. Полный тестовый набор на рабочем сервере ещё не подтверждён.

### V10.8.259 — обновлены тесты CBR пейсера и UDP/RTP/HTTP/File CBR

- После исправлений тестов remapper на V258 `media_core_tests` на Ubuntu остановился в `testCbrTsPacer(): assert(datagram[0] == mediaPacket)`. Это **устаревшее предположение**: текущий WISI CBR pacer использует токены полезного потока (первоначально 75% от TS bitrate) и распределяет NULL-пакеты между полезными слотами, а не только после них.
- В `tests/test_media_core.cpp` тест прямого pacer проверяет точное присутствие **одного медиа-пакета в любом слоте 7×188**, валидность всех пакетов и последовательный NULL continuity. Лимит FIFO теперь берётся из `CbrTsPacer::maximumQueuedBytes()` (8 MiB), а не устаревшей константы 2 MiB. Минимальный CBR bitrate также проверяется по действующей нижней границе 100 kbps (а не старой 512 kbps).
- Исправлены связанные интеграционные тесты UDP CBR, UDP+RTP+CBR fanout, File CBR, HTTP CBR, HTTP query key + CBR. Помощник `collectCbrMedia` проверяет размер каждой полученной датаграммы 7×188, синхробайт TS, NULL continuity, состав и порядок **всех полезных пакетов** независимо от смещения среди NULL-пакетов. Тестовые короткие потоки теперь используют `CbrTsPacer::kNoStartupBufferMarker` (+699), чтобы не ждать штатный 3-секундный накопительный буфер WISI; реальный transport target остается 512 kbps.
- Производственный `CbrTsPacer`, `WisiCbrOutputWorker`, `MpegTsRemapper`, `NativeMpegTsMux` и все декодеры/энкодеры **не менялись**. PCR preroll 500ms V256 сохранён для последующей проверки lip-sync на настоящем VLC.
- Проверка: `git pull --ff-only origin main && cmake -S tests/media_core -B build-test-media && cmake --build build-test-media --parallel 2 && ctest --test-dir build-test-media --output-on-failure`. Эти обновлённые тесты ещё должны быть запущены на Linux — локально полный набор нативных зависимости/ввода не тестировался.

### V10.8.258 — тест восстановления после фрагментированного PAT

- На Ubuntu `ctest` V257 завершался в `testMpegTsRemapper()` на старом ожидании `!remapper.process(fragmentedPat,...)`: оно не соответствует текущему `collectSections()`, где неполная PAT секция буферизуется между TS пакетами вместо немедленного завершения канала.
- Обновлён только `tests/test_media_core.cpp`: неполный PAT при continuity=4 должен быть принят без ошибки, следующий корректный PAT при continuity=5 должен восстановить PSI сборщик, а сервис SID=42 и ремаппинг video PID=0x0200 остаться действующими. Тест не зависит от случайного момента фоновой генерации PAT/PMT/SDT.
- Производственные `MpegTsRemapper.cpp` и `NativeMpegTsMux.cpp` не менялись; PCR decode preroll 500ms из V256 сохранён. Исправление только регрессионного теста, не новая проверка реальной синхронизации VLC.
- Команды: `git pull --ff-only origin main`, `cmake -S tests/media_core -B build-test-media`, `cmake --build build-test-media --parallel 2`, `ctest --test-dir build-test-media --output-on-failure`. Если тест проходит, собрать основное приложение и portable архив V258.
- `__int128` -Wpedantic и предупреждение об unused-function не вызвали предыдущего падения теста.

### V10.8.257 — исправление устаревшего теста MPEG-TS Remapper

- В V256 `media_core_tests` собирался, но `testMpegTsRemapper()` останавливался на `output.size() == 2`. Это был **не провал нового PCR теста**: прежняя проверка ожидала выдачу PAT/PMT сразу при получении PMT, хотя актуальный remapper делает `remapReady_=true` и выдаёт PAT/PMT/SDT на следующем периодическом `tick()` / входном TS-пакете.
- Обновлён только регрессионный `tests/test_media_core.cpp`: после PMT ожидается пустой вывод; `tick()` выдаёт PAT, PMT, SDT; проверяются PID, SID, remapped audio/video PID и CRC; также CAT, EMM, video PID и очередная PAT после 120 мс. Исходный PSI-continuity принадлежит remapper, а не входному TS.
- `src/media/MpegTsRemapper.cpp` **не изменён**. Исправление PCR=video DTS−500 мс из V256 в `NativeMpegTsMux` сохранено. Не менялись Nvenc/Nvdec, VAAPI/QSV, FFmpeg/Ittiam, потоковые выходы и интерфейс.
- Проверка на сервере: `cmake -S tests/media_core -B build-test-media && cmake --build build-test-media --parallel 2 && ctest --test-dir build-test-media --output-on-failure`. Если CTest проходит, пересобрать основное приложение с версией V257 и переносимый архив. Сборка теста после GitHub-фикса локально пока не подтверждена, нужна проверка на Ubuntu.

### V10.8.256 — MPEG-TS PCR decode preroll для всех декодеров (A/V sync)

- Пользователь подтвердил, что звук звучит раньше картинки при **любом доступном видеодекодере**, а не только FFmpeg. Поэтому настройка ручного аудио-офсета V255 **полностью отменена** из кода, UI, JSON конфигурации, clipboard и тестов: она не требуется, пользователь хочет автоматическую синхронизацию.
- Выявлен общий дефект: `NativeMpegTsMux::packetizePes()` записывал `PCR_base = video DTS`. При live-транскодировании без B-кадров обычно `PTS=DTS=PCR`, то есть MPEG-TS приемнику не предоставляется времени декодировать и отобразить кадр до его presentation timestamp. Аудио может начать звучать раньше видимого видео независимо от декодера или кодировщика.
- Исправление в общем MPEG-TS mux: `PCR_base=(video DTS − 45000) mod 2^33`, то есть **500 мс decode preroll**, без изменения исходных PTS/DTS видео и аудио и без принудительного lip-sync offset. Один PCR clock используется всеми выходными элементарными дорожками. Частота выдачи PCR не меняется (на каждом видео PES).
- Это **гипотеза общего корня**, основанная на анализе кода и симптомах на всех декодерах, пока не подтвержденная реальным VLC. Реальная A/V синхронизация и размер буфера могут также зависеть от приемника и mux pacing. Не меняли NVENC/NVDEC, VAAPI/QSV, Ittiam, FFmpeg PAL clock, абонентские output настройки, системный аудиодекодер и mux queue policy V254.
- В `tests/test_media_core.cpp` добавлены проверки 500-мс отставания PCR от video DTS и корректности 33-bit wrap. Запуск: `cmake -S tests/media_core -B build-test-media && cmake --build build-test-media --parallel 2 && ctest --test-dir build-test-media --output-on-failure`; при необходимости запуск `build-test-media/media_core_tests` напрямую.
- После установки V256 проверять VLC с любым декодером, начиная с рабочего FFmpeg→VAAPI, отсутствие `NATIVE AV MUX DROP` и `NATIVE ASYNC VIDEO RESYNC`. Если lip-sync не восстановился, измерять PTS/PCR фактического output TS, а не добавлять ручной сдвиг аудио. На фоне V254 `av_delta_ms` может оставаться 50–130 мс: эта метрика сравнивает дорожки, новый PCR-lag ее не изменяет.

### V10.8.254 — FFmpeg AVC: максимальное опережение видеопотока на mux 150 мс

- Диагностика V253 для канала `stream-1793338874706`: `input_av_ms` примерно +2064..2152 мс по последним меткам producer, `decoded_video_vs_input_ms` −240..−360 мс, `encoded_audio_vs_input_ms` около 0..45 мс; очередь видео 28..34 кадра и `mux_av_ms` +442..456 мс. Продолжительная близость к 500-мс пределу указывает на то, что планировщик выпускает видео почти на пределе допуска относительно уже отправленного аудио.
- Для **явно выбранного H.264 декодера `ffmpeg`** условие `withinHardLead` и ожидание lagging peer теперь ограничены тем же **150-мс preferred lead**, вместо прежнего 500-мс hard escape. Это ограничивает mux-time video ahead of audio без изменения медиатегов, декодирования, FPS-gate или audio PTS. Ожидаем `hard_lead_ms=150` в `NATIVE AV MUX`, `av_delta_ms` ближе к 150 мс (значение может колебаться при очередях).
- Для других выбранных декодеров, `auto`, Intel/VAAPI, NVENC/NVDEC и MPEG-TS output protocol сохраняется прежний 500-мс hard-lead. **Аудио PTS намеренно не сдвигаются**: Source PTS watermark 2.1s может отражать пакетную подачу/буферизацию; mux AV gap не доказывает 450ms воспринимаемого lip-sync.
- Проверить на live сервере VLC и `NATIVE AV MUX` + `NATIVE AV MUX DROP`: более строгое ожидание видео увеличит очередь примерно на несколько кадров. Если очередь достигает 64 или появляются `queue_full`, вернуться к V253 и анализировать источник. При отключенном аудио использовать `auto`/другой декодер, поскольку A/V lead-policy не нужен.
- Это рабочая гипотеза и целевое исправление планировщика, не проверенное на аппаратуре и не гарантирующее устранение полного source PTS mismatch. V251 обеспечила чистое изображение на FFmpeg, V252–253 стабилизировали PAL FPS/PTS; эти пути не изменены.

### V10.8.253 — FFmpeg PAL AV sync: убран искусственный PTS lead; стадийная диагностика

- По реальному журналу V252 `NATIVE AVC FFMPEG PTS cadence ... source_delta90k=-10800`, что показывает: старая коррекция привязывала дальнейшее видео к аномальному самому первому timestamp и добавляла 120 мс опережения. Новый lock на 25-м кадре привязывается к **текущему исходному FFmpeg PTS**, а следующие display-order кадры идут с PAL шагом 3600/90k. На момент переключения и при реальном discontinuity сбрасывается FPS-gate clock для предотвращения дополнительного пропуска кадров.
- Добавлена ограниченная режимом `FFmpeg` строка `NATIVE AV CLOCK TRACE` для сравнения последних **producer watermark** меток между источником, decoder, audio encoder и mux (`input_av_ms`, `decoded_video_vs_input_ms`, `encoded_audio_vs_input_ms`, `mux_av_ms`, `mux_video_vs_input_ms`, `mux_audio_vs_input_ms`). Это разные моменты обработки и НЕ измерение физической lip-sync задержки на экране; диагностические значения нужно оценивать вместе с очередями.
- При отсутствии меток числовая диагностика показывает `999999` как признак неизвестного значения. Для других выбранных видеодекодеров новый диагностический лог не выводится. Скорость аудио и исходные аудио PTS не меняются. Mux lead window 150/500 мс, Intel VAAPI encoder и NVIDIA NVENC/NVDEC оставлены неизменными.
- Добавлен регрессионный тест на начальную PTS аномалию -10800 тиков. `tests/test_ffmpeg_pal_pts_clock.cpp` компилируется отдельно командой из комментария в начале файла.
- **Важно:** фикс убирает доказанные искусственные 120 мс, но не гарантирует устранение полного AV-skew: V252 уже показывала около +450 мс на последних записанных mux PTS; необходимо сравнить новые `NATIVE AV CLOCK TRACE` и VLC после установки.

### V10.8.252 — FFmpeg 25fps PAL PTS clock stabilization

- В логах V251 на канале 8 FFmpeg чисто декодирует interlaced 720×576 25fps, но generic `NATIVE VIDEO FPS GATE` отбросил более 1000 изображений при входном PAL 25fps. Выходной AV mux показывал стабильные +300…+500ms (`av_delta_ms`), без переполнений mux.
- Только для **явно выбранного FFmpeg H.264**, интерлейсного выходного кадра и целевого **ровно 25fps** введён отдельный clock. После 25 последовательно выданных FFmpeg-кадров он сравнивает реальный PTS-диапазон с PAL cadence и включается лишь при соответствии (±120ms). Переупорядоченные/дрожащие PTS заменяются display-order PTS c шагом 3600/90k, до legacy `FPS GATE`; H264 пиксели и реальные sample count не меняются.
- Нестабильный/30/50 fps источник не проходит PAL eligibility и продолжает использовать стандартный FPS gate. При PTS discontinuity >1sec clock перенастраивается; лог `NATIVE AVC FFMPEG PTS discontinuity`. При lock лог `NATIVE AVC FFMPEG PTS cadence ... locked=1`.
- Есть отдельный тест `tests/test_ffmpeg_pal_pts_clock.cpp` для PAL jitter, 30fps bypass, 33-bit PTS wrap и discontinuity; компиляция теста: `g++ -std=c++17 -Isrc tests/test_ffmpeg_pal_pts_clock.cpp -o /tmp/test_ffmpeg_pal_pts_clock && /tmp/test_ffmpeg_pal_pts_clock`.
- Изменение не применяется к Ittiam, OpenH264, Intel oneVPL, NVIDIA NVDEC/NVENC, progressive AVC, AV mux, AAC и encoder. Дальнейшая runtime-проверка обязательна: `NATIVE VIDEO FPS GATE dropped` должен перестать расти после lock, а `av_delta_ms` требуется сравнить с V251, поскольку аудио PTS отдельно не сдвигались.

### V10.8.251 — независимый FFmpeg H.264 decoder для interlaced IPTV

- По скриншоту V250 видны сохраняющиеся вертикальные блочные артефакты на вещательном AVC 720×576 25 fps. Дамп того же канала успешно декодируется независимым FFmpeg, а NativeTsDemux уже правильно собирает пары полей. Поэтому добавлен отдельный selectable software H.264 decoder на libavcodec без замены Ittiam/OpenH264 или аппаратных энкодеров.
- В редакторе плитки: **Декодер H.264 → FFmpeg libavcodec** (также в английском UI). Новый декодер использует один worker для стабильной обработки PAFF/MBAFF, принимает H.264 Annex-B access units через FFmpeg send/receive API, копирует stride Y/U/V planes в I420, сохраняет frame PTS и interlaced-флаг, передаёт обычному CPU/VAAPI/QSV/NVENC encoder.
- Опциональные зависимости: `sudo apt-get install -y libavcodec-dev libavutil-dev pkg-config` и чистая переконфигурация CMake. При отсутствии dev-файлов старая сборка остаётся доступна, FFmpeg-пункт disabled. Проверить строку `Optional H.264 software decoder: FFmpeg/libavcodec enabled`.
- Portable bundle `scripts/make_portable_linux_bundle.sh` рекурсивно собирает ELF-библиотеки; в архиве также проверить libavcodec и libavutil.
- Устранение артефактов нужно подтвердить в VLC на канале 8 после установки: `FFmpeg libavcodec` decoder + `Intel VAAPI direct` encoder, 720×576, 25 fps без deinterlace. Ожидаются логи `NATIVE AVC DECODER backend=ffmpeg-libavcodec`, `NATIVE AVC FFMPEG OUTPUT`.
- NVENC/NVDEC, Intel zero-copy, VAAPI encoder, Ittiam, OpenH264, MPEG-TS output и остальные каналы не менялись. Режим явно выбирается пользователем и не становится глобальным.

### V10.8.250 — исправлена сборка interlaced AVC field pairs из IPTV MPEG-TS

- Диагностика канала 8 (`stream-1793338874706`) по дампу из V249 выявила, что поток выдаёт 50 отдельных top/bottom field NAL в секунду, причём каждый field предваряется собственными AUD/PPS. FFmpeg независимо декодировал видео с нормальными кадрами. Старый `NativeTsDemux::queueVideoSample()` немедленно резал AU по повторному AUD/PPS, до уже существующей проверки `complementaryField`; поэтому Ittiam получал отдельные field-AU вместо пары.
- В `NativeTsDemux.cpp` AUD/SPS/PPS после первого slice теперь являются отложенной границей. Если следующий slice — комплементарное поле с тем же `frame_num`/PPS и противоположным `bottom_field_flag`, оба поля и промежуточные NAL передаются Ittiam вместе. Если следующий slice — новое изображение, граница ставится перед его AUD/SPS/PPS, так что NAL не теряются.
- Учитываются GOP, где первое поле — IDR (NAL 5), а второе — non-IDR (NAL 1). Если оба IDR, проверка `idr_pic_id` сохраняется. В предоставленном дампе после появления SPS: 1749 полей; 874 последовательные пары соответствуют признакам пары, в том числе 17 переходов IDR/non-IDR.
- Логи `NATIVE AVC AU FIELD_PAIR` и `NATIVE AVC AU FIELD_CLOCK` позволяют подтвердить эффект на живом потоке: частота `FIELD_PAIR` должна вырасти, а `FIELD_CLOCK` почти исчезнуть.
- **Не менялись** NVENC/NVDEC, oneVPL, Intel VAAPI encoding, CPU scaler, прочие протоколы и структура сохранённого конфига. Сборка на реальном сервере и устранение артефактов требуют проверки после установки.

### V10.8.249 — адресный дамп H.264 для диагностики артефактов Ittiam

- Исходный interlaced AVC 720×576 продолжает содержать артефакты даже без deinterlace и масштабирования; цветной тестовый I420-кадр проходит CPU scaler → Intel VAAPI encoder без повреждений. Поэтому нельзя считать причиной Intel encoder до проверки входных AVC байтов.
- Ittiam error `0x00000091` соответствует `ERROR_START_CODE_NOT_FOUND`; одного этого кода недостаточно, чтобы объявить входное видео повреждённым.
- Ранее `DVBSTREAMER5_DUMP_H264_ES=/tmp/dvb-8.h264` писал до 64 MiB для каждого AVC потока. Для безопасности теперь требуется ещё `DVBSTREAMER5_DUMP_H264_ES_STREAM_ID=stream-1793338874706`. Без точного stream_id дамп не выполняется. Файлы включают stream_id, PID, индекс с PTS/DTS и не обрезают access unit на границе лимита.
- Пример systemd drop-in: `Environment=DVBSTREAMER5_DUMP_H264_ES=/var/tmp/dvb8.h264` и `Environment=DVBSTREAMER5_DUMP_H264_ES_STREAM_ID=stream-1793338874706`. После перезапуска службы файл `/var/tmp/dvb8.<stream-id>.<hash>.pid-*.h264` можно проверить независимым декодером ffmpeg/VLC.
- **Это инструмент диагностики, а не исправление артефактов.** Потоковый путь Ittiam, Intel VAAPI, NVIDIA NVENC/NVDEC и база V241 остались неизменными.

### V10.8.248 — VAAPI VPP bypass guard и диагностика PAL upscaling

- По runtime-логу 720×576 → 1920×1080 обнаружено, что выбранный OpenH264 не выдаёт кадры и автоматически переключается на Ittiam после 150 samples; это не чистое сравнение двух software decoders.
- Добавлена дополнительная защита прямо в `VaapiH264Encoder::encode`: когда входной размер отличается от выходного, по умолчанию выполняется CPU I420 scaling и только потом аппаратное VAAPI H.264 encode, даже если вызывающий код не выполнил scaler negotiation. Внутренний VAAPI VPP для I420 применяется только с явным `DVBSTREAMER5_ENABLE_VAAPI_SOFTWARE_SCALE=1`.
- Добавлен обязательный startup-лог `NATIVE VAAPI ENCODER scale-policy` с реальным source/output и выбранным CPU/VPP режимом, чтобы отличить обновлённый бинарник от старого.
- Существующие Intel hardware decode и NVIDIA NVDEC/NVENC реализации не изменены. Настройку `OpenH264` пока сохраняем с fallback для обратной совместимости, но реальный decoder надо проверять по `NATIVE AVC DECODER`.
- Само исчезновение артефактов пока не подтверждено тестом на сервере. Для диагностики есть уже существующая per-stream подмена raw-I420 кадра через `DVBSTREAMER5_DIAG_COLOR_STREAM_ID`.

### V10.8.247 — исправлен обход CPU scaler в legacy VAAPI encoder

- При использовании Intel `VAAPI direct` c программным H.264 decoder прежний `supportsNativeScaling()` ошибочно включал встроенный VAAPI VPP при изменении разрешения, обходя исправление V243. Теперь по умолчанию CPU I420 scaler выполняется до VAAPI encoder, а H.264 кодируется аппаратно.
- Для сравнения допускается явное включение старого GPU VPP: `DVBSTREAMER5_ENABLE_VAAPI_SOFTWARE_SCALE=1`. На Haswell/i965 с наблюдаемыми артефактами включать его не рекомендуется.
- Сохранена диагностика V246: Intel oneVPL HW decoder отключается в UI, если нет аппаратной реализации API 2.1+. Для Haswell/i965 отдельный VAAPI VLD decoder пока не реализован. Доступность Intel VAAPI encode не зависит от oneVPL decode.
- NVIDIA NVDEC/NVENC код не менялся; база V241 и Intel GPU metrics сохранены.

### V10.8.246 — исправлена диагностика возможностей Intel HW decoder

- `Intel HW / oneVPL` доступен в UI только при подтверждённой аппаратной реализации oneVPL API 2.1+ (обязательной для `FrameInterface::Map/Unmap`). Наличие отдельного Intel VAAPI encoder больше не выдаётся за доступность oneVPL decoder.
- На legacy Intel Haswell с рабочим `i965` аппаратное кодирование `VAAPI direct` остаётся доступно. Ручной oneVPL decoder отключён, поскольку для старого i965 нужен отдельный native VAAPI VLD decoder, который здесь ещё не реализован.
- Если oneVPL недоступен, вручную выбранный `intel_hw` выдаёт понятную ошибку вместо неясного `OFFLINE`; сохранённые конфигурации автоматически не переписываются.
- Режимы NVIDIA/NVDEC/NVENC не менялись. Исправление Map/Unmap из V245 сохранено для поддерживаемых oneVPL 2.1+ GPU.

### V10.8.245 — oneVPL Intel HW decoder: корректное отображение поверхности

- Исправлен ручной Intel oneVPL hardware decoder: после `DecodeFrameAsync` decoded surface синхронизируется и обязательно отображается через `FrameInterface->Map(MFX_MAP_READ)` перед чтением `Data.Y/Data.UV`. После копирования I420 выполняются `Unmap` и `Release`.
- Проверяются формат NV12, coded/visible geometry, чётность crop, Y/UV pointers и pitch. При ошибке выводится подробная диагностика вместо общего сообщения.
- Это исправляет чтение неотображённой oneVPL поверхности в V244. Не устраняет отсутствующий Intel hardware runtime/driver: для такого случая нужен полный журнал `Intel HW decoder...`.
- NVIDIA NVDEC/NVENC и штатный Intel zero-copy pipeline не изменены.

### V10.8.244 — настоящий encoder-independent Intel HW decoder

- Ручной режим `Intel HW / oneVPL` больше не требует encoder `QSV/Auto`. Добавлен отдельный oneVPL hardware decoder, который синхронизирует decoded surface, учитывает реальный NV12 pitch и crop, преобразует видимую область в I420 и передаёт кадр выбранному encoder-у.
- Поддерживаются комбинации Intel HW decode → CPU, QSV, VAAPI или NVENC encode. Это устраняет прежний OFFLINE с сообщением `Intel HW decoder...` при несовместимой жёсткой привязке к zero-copy transcoder.
- Автоматические Intel/NVIDIA zero-copy пути сохранены как отдельные оптимизации. Ручной NVIDIA HW пока остаётся связан с NVDEC → CUDA → NVENC.
- Сохранены исправления V243: CPU scaler по умолчанию для software-decoded I420 и startup gate по реальным random-access NAL.

### V10.8.243 — software scaler corruption fix и QSV startup fix

- Исправлен общий путь software-decoded I420 кадров: автоматический VAAPI VPP scaler больше не используется по умолчанию. OpenH264/Ittiam/обычный software decode теперь масштабируются встроенным CPU I420 scaler-ом перед CPU/NVENC/QSV encode. Это изолирует наблюдавшиеся блочные/полосовые артефакты от Intel VAAPI VPP. Для A/B VAAPI software scaling можно вернуть только явно: `DVBSTREAMER5_ENABLE_VAAPI_SOFTWARE_SCALE=1`.
- Dedicated hardware paths не отключены: `NVIDIA HW / NVDEC` по-прежнему использует NVDEC → CUDA → NVENC, `Intel HW / oneVPL` — Intel oneVPL decode/VPP/encode zero-copy.
- Исправлен startup gate QSV/oneVPL: готовность видео теперь определяется по фактическим H.264/H.265 random-access NAL и накопленным parameter sets, а не требует encoder `FrameType` и SPS/PPS/IDR одновременно в одном AU. Добавлены логи `NATIVE VIDEO STARTUP waiting/ready`.
- База V241 и Intel GPU header metrics сохранены.

### V10.8.242 — ручной выбор аппаратного H.264 decoder на базе V241

- База V241 сохранена: Intel GPU process Video/Render metrics, автоматические transcode-настройки V240 и остальные изменения ветки `intel-gpu-metrics-v10.8.241` не откатывались.
- В редактор транскодирования добавлены режимы `NVIDIA HW / NVDEC` и `Intel HW / oneVPL` рядом с `Auto`, `Ittiam libavc` и `OpenH264`.
- `NVIDIA HW / NVDEC` принудительно использует существующий NVDEC → CUDA → NVENC zero-copy путь и требует encoder `NVENC` или `Auto`.
- `Intel HW / oneVPL` принудительно использует существующий Intel oneVPL decode/VPP/encode zero-copy путь и требует encoder `QSV` или `Auto`.
- При недоступном hardware runtime ручной режим завершается явной ошибкой вместо скрытого fallback на software decoder. Реализации NVENC/NVDEC и Intel encoder не переписывались.

### V10.8.241 — Intel GPU utilization in the header (QSV / VAAPI)

**Test branch:** `intel-gpu-metrics-v10.8.241`, based on `transcode-auto-v10.8.240`. `main` remains untouched.

- **Root cause:** NVIDIA NVML supplies a card-wide `utilization_percent`, while Intel i915/Xe DRM GPU cards often expose no compatible global `gpu_busy_percent`; the old header therefore displayed "N/A" despite ongoing Intel hardware transcoding.
- **Intel live fix:** when card-wide GPU percentage is unavailable, calculate DVBStreamer5 **process-owned** utilization from DRM per-engine counters and display `GPU 45% · Intel (app)` in the header, with an explicit app-only scope and engine name in the tooltip. This is a percentage of the busiest measured GPU engine and is **not** whole-card utilization. Do not invent 0% when counters are unavailable.
- **Hardware compatibility:** Intel `i915` uses `/proc/self/fdinfo` `drm-engine-video/render` nanoseconds; Intel `xe` uses `drm-cycles-*` divided by `drm-total-cycles-*` over the sample interval. Both use device PCI ID matching. The 3-second header sampler and 30-second history sampler have separate baselines. No external `intel_gpu_top`, `nvidia-smi`, root privileges or shell commands are required for the sampling itself.
- **NVIDIA unchanged:** NVML card-wide utilization keeps priority when present; mixed Intel/NVIDIA systems choose the largest available figure and identify Intel's process-specific fallback.
- **System load panel:** the per-device percentage also falls back to app DRM GPU busy with clear source labeling; existing GPU graphs and separate per-process engine histories remain available.
- **No transcoder changes:** CPU, NVENC, NVDEC, QSV, VAAPI, mux, OSCam and DVB source/CA pipelines are unchanged. All automatic transcoding configuration from V10.8.240 is retained.
- **Tests:** `node tests/web/test_intel_gpu_header.js`, plus `bash scripts/test_auto_transcode_policy.sh`. The CI performs a CPU-only CMake compile. Intel hardware/transcode output must still be verified on the actual server.

Ubuntu 22.04 / 24.04 source update from a separate test checkout:

```bash
git fetch origin
git switch intel-gpu-metrics-v10.8.241 || git switch -c intel-gpu-metrics-v10.8.241 --track origin/intel-gpu-metrics-v10.8.241
git pull --ff-only
node tests/web/test_intel_gpu_header.js
bash scripts/test_auto_transcode_policy.sh
bash scripts/vendor_srt_source.sh
bash scripts/vendor_native_codecs.sh
DVBSTREAMER5_CODEC_JOBS=2 bash scripts/build_native_codecs.sh
cmake -S . -B build-v241 -DCMAKE_BUILD_TYPE=Release \
  -DDVBSTREAMER5_ENABLE_NVENC=ON \
  -DDVBSTREAMER5_ENABLE_INTEL_QSV=ON \
  -DDVBSTREAMER5_ENABLE_VAAPI=ON
cmake --build build-v241 --parallel 2
bash scripts/make_portable_linux_bundle.sh --build-dir build-v241 --binary build-v241/DVBStreamer5 --output-dir dist/DVBStreamer5-linux-x86_64 --with-oscam --archive
```

While an Intel QSV/VAAPI channel is transcoding, examine API metrics without changing the running daemon:

```bash
curl -fsS http://127.0.0.1:9000/api/system-metrics | jq '{gpus,process_gpus}'
```

If the API requires session authentication, use your normal authenticated browser/network route and inspect the JSON response. Wait for at least two live metrics polls; the first has no previous DRM counter baseline. Unsupported kernel DRM statistics legitimately display N/A rather than a fabricated percentage.

---

### V10.8.240 — automatic video-transcoding controls (CPU / NVIDIA / Intel)

**Test branch:** `transcode-auto-v10.8.240`, based on `dvb-vbr-null-v10.8.239`; `main` is not modified.

- **Simplified stream editor:** Video codec (including Copy), encoder (Auto/CPU/NVIDIA NVENC/Intel QSV/Intel VAAPI), H.264 decoder, output resolution, video bitrate, FPS, deinterlace, audio controls and HLS ABR remain user-selectable.
- **Advanced controls removed:** no manual Preset, Profile, Level, GOP, reference frames, AQ strength, B-frames or Open GOP. `Profile=auto` and `Level=auto` are enforced for all backends; the encoder negotiates the effective hardware-supported profile/level.
- **Automatic policy:** the explicitly configured **transcoding video bitrate** (not the TS/UDP-CBR bitrate), output resolution and FPS determine a supported real-time speed/quality preset and adaptive quantization; GOP is approximately 2 seconds (`round(fps * 2)`). Live video uses B-frames 0, refs auto, Open GOP off. On high-throughput 1080p50/60 use a faster preset, and CPU 4K50/60 uses fastest. This does **not** change the user-specified bitrate, frame rate, resolution, audio or encoder choice.
- **CBR-independent:** automatic preset/GOP/AQ use the bitrate entered under **Transcoding → Video bitrate**, even if a separate MPEG-TS CBR output limit constrains the effective encoder bitrate. Transport CBR remains independent. Each lower HLS ABR rendition is automatically tuned using its own configured rendition video bitrate.
- **Migrates older configs:** legacy manually saved advanced properties are ignored by config load and stream API save; the new automatic values are stored for consistent UI/API state. Existing transcoding copy/paste transfers only user-visible controls. Original copy/pass-through and non-transcoded streams are unaffected.
- **Tests:** `bash scripts/test_auto_transcode_policy.sh`; also part of the experimental branch's CI. A successful CI build, actual NVIDIA/Intel encoding and hardware playback must be verified independently.

Ubuntu 22.04 / 24.04 — new source checkout, CPU and any detected GPU backends:

```bash
git clone --recurse-submodules -b transcode-auto-v10.8.240 https://github.com/Monkipnet/DVBStreamer5.git DVBStreamer5-v240
cd DVBStreamer5-v240
bash scripts/vendor_srt_source.sh
bash scripts/vendor_native_codecs.sh
DVBSTREAMER5_CODEC_JOBS=2 bash scripts/build_native_codecs.sh
bash scripts/test_auto_transcode_policy.sh
cmake -S . -B build-v240 -DCMAKE_BUILD_TYPE=Release \
  -DDVBSTREAMER5_ENABLE_NVENC=ON \
  -DDVBSTREAMER5_ENABLE_INTEL_QSV=ON \
  -DDVBSTREAMER5_ENABLE_VAAPI=ON
cmake --build build-v240 --parallel 2
bash scripts/make_portable_linux_bundle.sh --build-dir build-v240 --binary build-v240/DVBStreamer5 --output-dir dist/DVBStreamer5-linux-x86_64 --with-oscam --archive
```

If the build host has Intel QSV or VAAPI headers/drivers, install `libvpl-dev` and `libva-dev` beforehand. GPU backends require their respective runtime drivers and hardware. To update a **separate test clone**:

```bash
git fetch origin
git switch transcode-auto-v10.8.240 || git switch -c transcode-auto-v10.8.240 --track origin/transcode-auto-v10.8.240
git pull --ff-only
```

Inspect startup logs for `NATIVE TRANSCODE AUTO stream=...` to confirm `configured_video_kbps`, `effective_video_kbps`, FPS, preset, GOP and automatic profile. Do not replace a working production binary or restart its service until compiling and testing this branch.

---

### V10.8.239 (ISOLATED DVB VBR NULL-PACKET FIX)

**Branch:** `dvb-vbr-null-v10.8.239`, directly based on experimental `ca-irdeto-v10.8.238`; stable `main` is unchanged.

- **Fix:** The shared DVB selected-service prefilter always strips *source-transponder* NULL packets (PID `0x1FFF`), regardless of CBR/VBR mode. Previously it stripped NULL packets only for CBR, so selecting UDP-VBR could send tens of Mbit/s for a ~2.7 Mbit/s service.
- **CBR unchanged:** The CBR worker generates its own NULL packets to meet the configured target bitrate. The fix does not affect full-transponder/MPTS passthrough, other inputs, OSCam, Irdeto CW buffering or DVB-CSA.
- **WISI Chameleon:** For controlled comparison, keep working CBR output at its known rate first. Then switch **one** selected DVB service to UDP-VBR and verify its measured *output* rate is close to its non-NULL service rate (not the full transponder rate). Watch WISI input-buffer/overflow alarms and RF DVB-C BER separately. VBR can still have bursts; CBR remains preferable if the downstream receiver requires a paced CBR input.
- **Status:** GitHub branch source patch; compile/CI results and live WISI results must be verified independently.

Build on Ubuntu (from source):

```bash
git clone --recurse-submodules -b dvb-vbr-null-v10.8.239 https://github.com/Monkipnet/DVBStreamer5.git DVBStreamer5-v239
cd DVBStreamer5-v239
bash scripts/vendor_srt_source.sh
bash scripts/vendor_native_codecs.sh
DVBSTREAMER5_CODEC_JOBS=2 bash scripts/build_native_codecs.sh
cmake -S . -B build-v239 -DCMAKE_BUILD_TYPE=Release
cmake --build build-v239 --parallel 2
bash scripts/make_portable_linux_bundle.sh --build-dir build-v239 --binary build-v239/DVBStreamer5 --output-dir dist/DVBStreamer5-linux-x86_64 --with-oscam --archive
```

Update existing clone:

```bash
git fetch origin
git switch dvb-vbr-null-v10.8.239 || git switch -c dvb-vbr-null-v10.8.239 --track origin/dvb-vbr-null-v10.8.239
git pull --ff-only
```

Confirm VBR filtering in application logs: `SHARED DVB service prefilter enabled ... drop_source_null=1`. That line must appear for a DVB service with CBR **off**, as well as for CBR **on**.

---
### V10.8.238 (ISOLATED DIAGNOSTIC BRANCH): simultaneous Irdeto / TS integrity monitoring

**Branch:** `ca-irdeto-v10.8.238` is based directly on `ca-irdeto-v10.8.236` (NOT V237). `main` is unchanged. It does not include V237 CHID preference caching. This version is for diagnosing occasional picture breakup on eight concurrent satellite services sharing one physical Irdeto card; it does **not** claim to repair the corruption.

**Opt-in:** add `DVBSTREAMER5_CA_TS_MONITOR=1` only for a controlled test of this build. Disabled by default; neither the CW algorithm nor service lifecycle is modified by the monitor. The independent V236 `DVBSTREAMER5_IRDETO_CW_BUFFER=1` remains available; when comparing old/new monitoring conditions, keep that flag at a consistent setting.

- **Input TS before decryption:** per-selected PID continuity-counter gaps, repeats, TEI, invalid adaptation fields, PCR backwards and >1-second jumps (heuristics), explicit discontinuity indicators, scrambled-packet totals and EVEN/ODD TSC changes. Independent continuity state per PID prevents conflating interleaved elementary streams.
- **Immediately after in-place DVB-CSA (still inside CA plugin):** packets whose TSC remains set, missing-key packets, video PES start code/header sanity and H.264/H.265 Annex-B NAL header syntax. A malformed NAL header is a *heuristic*, not proof that an entire frame is incorrect. PES NAL scan is limited to AVC/HEVC PIDs identified in PMT and does not perform complete access-unit or decoder validation.
- **ECM/CW correlation:** completed valid Newcamd CW response latency, responses slower than 500 ms, peak latency and no-CW response counts; log per successful transaction (`CA V238 CW EVENT`) with SID, CHID, parity, duration, action and **never the CW bytes**. CA V236 normal CHID selection/rejection events remain available.
- One journal line per monitored service per second: `CA V238 MONITOR: stream=... sid=... in_cc_gaps=... post_pes_bad=... nal_bad=... ecm_over_500ms=...`. Counters are interval deltas (except `ecm_peak_ms_total`). The aggregate is per-service, allowing all eight channels to be observed without eight TVs.
- **Limitations:** the CA plugin sits *after* the transport source's selection and *before* final remuxing, rate pacing, SRT/UDP/HTTP output and the client decoder. It cannot prove the original RF input has no errors, determine packet loss on a viewer's network, verify every encrypted byte, or assert that all detected NAL anomalies are real picture breakup. Correlate with DVB tuner signal readings and a client-side TS capture when investigating an anomaly. A CC repeat can be a harmless transport duplicate.
- **CPU impact:** while enabled, every selected 188-byte TS packet is inspected twice (pre/post CSA) with small per-PID state; write volume ~8 summary lines per second, plus CW events on eight active services. Disable for continuous production operation.
- **Unit/CI tests:** `bash scripts/test_irdeto_cw_policy.sh`; `bash scripts/test_ts_integrity_monitor.sh`. Existing `.github/workflows/irdeto-v236-ci.yml` workflow is extended to cover V238 and attempt CPU-only full CMake compilation. CI cannot emulate a physical Irdeto smartcard or DVB-S2 receiver.
- Build on a separate Ubuntu host, or in a separate directory on `iptv` without touching installed production files:

```bash
git clone -b ca-irdeto-v10.8.238 https://github.com/Monkipnet/DVBStreamer5.git DVBStreamer5-v238
cd DVBStreamer5-v238
bash scripts/test_irdeto_cw_policy.sh
bash scripts/test_ts_integrity_monitor.sh
bash scripts/vendor_srt_source.sh
bash scripts/vendor_native_codecs.sh
DVBSTREAMER5_CODEC_JOBS=2 bash scripts/build_native_codecs.sh
cmake -S . -B build-v238 -DCMAKE_BUILD_TYPE=Release
cmake --build build-v238 --parallel 2
bash scripts/make_portable_linux_bundle.sh --build-dir build-v238 --binary build-v238/DVBStreamer5 --output-dir dist/DVBStreamer5-linux-x86_64 --with-oscam --archive
```

- **Read-only monitor retrieval after explicitly authorized deployment and one controlled service restart:**

```bash
journalctl -u dvbstreamer5.service --since '-20 min' -o short-iso --no-pager |
  grep -E 'CA V238 MONITOR:|CA V238 CW EVENT:|NEWCAMD IRDETO CHID' > /tmp/v238-ca-monitor.log
python3 scripts/analyze_ca_ts_monitor.py /tmp/v238-ca-monitor.log
```

**DO NOT copy the test binary/plugin over the live `/opt/DVBStreamer5` files or restart `dvbstreamer5.service` on `iptv` during routine build.** For a maintenance deployment, back up the binary, newcamd .so, config and service drop-ins and define rollback before restarting; only the operator should authorize the restart. Both CA plugin and matching main binary must come from the same build.

---

### V10.8.236 (TEST BRANCH) — per-service Irdeto CW buffer for single-card / multi-channel setups

- **Not a production release.** Developed separately on `ca-irdeto-v10.8.236`; `main` (V10.8.235) is unchanged. No reboot or restart of the production `iptv` host is required.
- An OSCam-mini instance using **one physical Irdeto smartcard** can serve multiple distinct encrypted services over per-service Newcamd TCP connections. Even when their reader is shared, EVEN/ODD control words are **never** shared between different service IDs. This implementation keeps a separate key pair and next-key buffer for each `ServiceBinding`.
- There was already a one-entry per-service exact ECM signature de-duplication and an in-flight ECM queue. A cache cannot predict future unknown CWs; the card still needs to process each new ECM. The ~153–176 ms observed at OSCam is normal for this deployment and not by itself proof of a CA problem.
- New **opt-in test mode** `DVBSTREAMER5_IRDETO_CW_BUFFER=1`: retain an active parity's current key while an updated CW is received; stage its next CW and activate after a TSC parity change returns to that parity. Flush outstanding CSA bitslice batches before switching keys. This is a *hypothesis* to diagnose brief 1–2 second visual breakup and **must be compared with the baseline**: some ECM/CAS timing may require different switching semantics.
- In opt-in mode, ignore all-zero Irdeto CW halves instead of replacing a previously working key. Skip unchanged CWs for a valid key. Non-Irdeto CAS paths and V10.8.235 behavior are untouched when the switch is unset.
- OSCam-mini UI activity indicator fix (independent of the experimental CW buffer): a local reader such as `Pervy1` is marked active when a successful `found (...) by Pervy1` ECM appears in a separate **last-45-second** journal slice. Previously the UI relied only on the startup-only `ready for requests` message, which rolls out of the last 240 lines on a busy eight-channel reader and falsely shows `Ожидание карты` even while decrypting. No OSCam settings or CA decryption logic are changed by this UI-only correction.
- With `DVBSTREAMER5_CA_DIAGNOSTICS=1`, log `NEWCAMD IRDETO CW BUFFER` and `NEWCAMD IRDETO CW ACTIVATE`, plus cumulative `irdeto_staged`, `irdeto_activated`, `irdeto_zero_skips`, and `irdeto_unchanged_skips` counters in `CA TS DIAG`. Only parity, SID, timing, and counters are logged; **never** control-word bytes.
- Autonomous regression test for the exact policy integrated into the Newcamd plugin: `bash scripts/test_irdeto_cw_policy.sh`. Tests are CPU-only, synthesize parity/CW transitions, need no smartcard/OSCam or tuner, and explicitly verify that CW caches from two services cannot overwrite each other. The standalone test was compiled and passed locally with GCC; this **does not** constitute a full DVBStreamer5 binary build or real-stream Irdeto validation.
- Isolated GitHub Actions workflow: `.github/workflows/irdeto-v236-ci.yml` runs the standalone policy test and separately attempts a CPU-only full DVBStreamer5 CMake build on this test branch. Its result is **not yet verified** here.
- **UI-only reader status fix:** check recent successful ECMs for a reader even when startup `ready for requests` rolled out of a short journal window. This fixes misleading `Ожидание карты` for the active `Pervy1` reader; no reader reset, OSCam restart or change of CA logic.
- Build on an isolated test server: `git fetch origin && git switch ca-irdeto-v10.8.236 && git pull --ff-only`, then use the project CMake and portable-bundle commands below. For test A/B set environment variables **only for the test process**. Do not install the test build or add systemd overrides to production `iptv`.
- Expected verification: parity transitions, PES validation, continuity counters, normal video and audio, same 8 services, and no new regressions. **No completed build or runtime video test is claimed for V10.8.236.**

### V10.8.235 — исправлена цветность NVDEC 1080i H.264 при сохранении GPU zero-copy

- **Подтверждено пользователем на GTX 1050, DAZN 1/2:** в V10.8.234 режим `NVDEC → CUDA(NV12) → NVENC`, `bob`, `second_field=0` устранил зелёный оттенок и цветные горизонтальные полосы. Журнал показал `host_video_copies=0`, `gpu_surface_copies=0`.
- Рабочая комбинация теперь **выбирается автоматически** при наличии чересстрочного H.264 и включённого деинтерлейсинга: `NVDEC Bob` + первое поле (`CUVIDPROCPARAMS.second_field=0`). В отличие от V10.8.234, `DVBSTREAMER5_NVDEC_FIRST_FIELD_TEST=1` и `DVBSTREAMER5_NVDEC_VPP_MODE=bob` **больше не нужны для этого сценария**.
- Для прогрессивного контента и других кодеков сохранён прежний режим Adaptive по умолчанию. Явный `DVBSTREAMER5_NVDEC_VPP_MODE=adaptive|bob|weave` имеет приоритет; выключенный деинтерлейсинг в плитке принудительно сохраняет Weave. Выбор первого поля по умолчанию активен для чересстрочных H.264-кадров при Bob (включая явно выбранный Bob).
- Логи: `NATIVE NVIDIA ZERO-COPY active ... deinterlace=bob auto_h264_bob=1 ... host_video_copies=0 gpu_surface_copies=0`. При `DVBSTREAMER5_NVDEC_VPP_PROBE=1` дополнительно `field_policy=first_h264_bob second_field=0` для чересстрочных H.264 кадров.
- **После установки удалить временные systemd overrides** из прошлых диагностических тестов (после сохранения их резервной копии): `30-nvdec-safe-copy.conf`, `40-nvdec-vpp-test.conf`, `90-nvdec-uv-debug.conf`, `98-h264-capture.conf`, `99-nvenc-input-test.conf`, `zz-nvdec-field-ab.conf` в `/etc/systemd/system/dvbstreamer5.service.d`; затем `systemctl daemon-reload && systemctl restart dvbstreamer5.service`. Это возвращает обычную работу и исключает случайное включение диагностического `NVDEC→CPU(NV12)→NVENC`.
- Пути Ittiam → NVENC, Intel VAAPI/QSV, обычный NVENC, OSCam-mini, EPG, subtitles и TS mux не менялись. Итоговую сборку V10.8.235 и воспроизведение **без** overrides следует отдельно проверить на сервере.

### V10.8.234 — A/B-проверка поля для NVDEC Bob по образцу FFmpeg CUVID

- Сравнение одного и того же H.264 DAZN 1 (PID 2047) через `ffmpeg -c:v h264_cuvid -deint bob -drop_second_field 1 -resize 1280x720` и программный FFmpeg показало близкие RGB: общая средняя абсолютная разница 8.80/255, средние цвета по 10 горизонтальным зонам почти одинаковы. Это не гарантирует отсутствия узких полос, но аппаратный CUVID способен вывести изображения с почти той же средней цветностью.
- Анализ исходников установил конкретную разницу: FFmpeg при `-drop_second_field 1` подаёт в `cuvidMapVideoFrame` `CUVIDPROCPARAMS.second_field=0`, а штатный путь DVBStreamer5 (по примеру NVIDIA NvDecoder) — `repeat_first_field + 1` (обычно 1). Это **гипотеза**, а не подтверждённое исправление дефекта.
- Добавлен безопасный отключаемый A/B режим `DVBSTREAMER5_NVDEC_FIRST_FIELD_TEST=1`: только при выбранном deinterlace и VPP режиме Bob/Adaptive принудительно `second_field=0`. По умолчанию поведение не изменено. Проверка осуществляется на одном канале, а в логе `NATIVE NVIDIA ZERO-COPY active` будет `nvdec_first_field_test=1`.
- Чтобы тест был чистым, одновременно установите `DVBSTREAMER5_NVENC_SOFTWARE_INPUT_TEST=0`, `DVBSTREAMER5_NVDEC_SAFE_COPY=0`, `DVBSTREAMER5_NVDEC_DIAG_CHROMA=off`, `DVBSTREAMER5_NVDEC_VPP_MODE=bob`; временно отключите захват H.264. После визуального A/B сравнения первого поля удалите тестовый systemd override и перезапустите сервис.
- Обычные Ittiam → NVENC, Intel, OSCam-mini, EPG, subtitles и оптимизация mux не менялись. Успешность полного CMake build и исправления цвета на GTX 1050 требует проверки на сервере.

### V10.8.233 — исправление компиляции захвата H.264

- Исправлена регрессия V10.8.232 в `src/media/NativeTranscoderPipeline.cpp`: восстановлен расчёт `videoDuration90k` (`90000.0 / fps`), удалены посторонние токены после диагностической функции захвата. Эта ошибка ранее приводила к синтаксическим ошибкам и множеству вторичных сообщений C++ компилятора.
- Сохранён раздельный захват H.264 по `streamId` и PID без обрезки отдельного access unit.
- Обычные NVDEC/NVENC/Ittiam, OSCam-mini, паковка TS и A/V mux этим коммитом не изменялись.
- Проверка: `git pull --ff-only origin main`, затем `cmake --build build-release --parallel 2` и `strings build-release/DVBStreamer5 | grep 'split_per_stream=1'`. После подтверждения компиляции собирайте portable обычным скриптом.

### V10.8.233 — исправление многоканального захвата H.264 для FFmpeg

- `DVBSTREAMER5_DUMP_H264_ES=/tmp/dazn-input.h264` теперь создаёт **отдельные** файлы для каждой пары `streamId + video PID` (например, `/tmp/dazn-input.sat-....pid-2047.h264`), не смешивая два канала в один некорректный H.264 elementary stream. В журнале показывается точный путь `NATIVE AVC ES DUMP open path=...`.
- Ограничение **64 МиБ на канал**, без обрыва посередине H.264 access unit. Файл `*.h264.index.csv` сохраняет метки PTS/DTS и идентификатор канала/PID.
- Диагностический режим по умолчанию выключен и не меняет обработку NVDEC, NVENC, Ittiam, MPEG-TS или A/V. Предыдущий 64 МиБ файл, записанный при двух активных каналах в V10.8.231, **не подходит для заключения о повреждении исходного H.264**, поскольку мог содержать перемешанные access units разных каналов.
- Проверка на сервере: `ls -lh /tmp/dazn-input*.h264`; затем `ffmpeg -hide_banner -loglevel warning -f h264 -i "<один файл PID>" -an -f null -`. Начало захвата может приходиться на середину GOP, поэтому отдельные ошибки в начале файла возможны; оценивайте ошибки после первого IDR.
- Точная ссылка на HLS-выход канала отличается от исходного DVB/IPTV; для независимой диагностики важно сравнить *входные* Annex-B bytes с выходом NVENC и исключить запись двух источников в один файл.


### V10.8.233 — сравнение входного NVENC буфера с программным путём

- Для диагностики зелёных/красных полос: `DVBSTREAMER5_NVENC_SOFTWARE_INPUT_TEST=1` оставляет аппаратный NVDEC/deinterlace/scale, но загружает Y и UV плоскости NV12 в RAM и заполняет NVENC-owned `NV_ENC_CREATE_INPUT_BUFFER` построчно по реальному `nvEncLockInputBuffer.pitch`. Это тот же способ подачи кадра в NVENC, который использует рабочий Ittiam/I420→NV12 путь; входной decoder при этом **не переключается на Ittiam**.
- По умолчанию режим отключён, основной NVDEC→CUDA→NVENC остаётся прежним. Включать только для A/B теста одного канала: передача GPU→RAM повышает загрузку CPU/PCIe и **не является production-оптимизацией**. Выключить после теста.
- Если цвет становится нормальным, исследовать CUDA resource registration/mapped NV12 pitch в zero-copy. Если полосы сохраняются, исследовать NVDEC-decode status=9, входные H.264 access units и исходную NV12 цветность. Без результатов теста исправление цветности **не подтверждено**.

# DVBStreamer5

**DVBStreamer5** — Linux-приложение для приёма, обработки, декодирования, транскодирования и раздачи DVB/IPTV-потоков с собственным native media engine и встроенным Web UI.

Текущая версия: **10.8.233**

> Проект не использует FFmpeg/libav или GStreamer как media framework. Транспорт, MPEG-TS mux/demux, HLS/CMAF, SRT, RTSP, RTMP, remap, EPG и основная логика транскодинга работают внутри DVBStreamer5. Кодеки подключаются как зафиксированные project-local библиотеки.

---

## Содержание

- [Основные возможности](#основные-возможности)
- [Поддерживаемые входы и выходы](#поддерживаемые-входы-и-выходы)
- [DVB-S/S2](#dvb-ss2)
- [Транскодирование](#транскодирование)
- [HLS / CMAF / ABR / DVR](#hls--cmaf--abr--dvr)
- [EPG и DVB EIT](#epg-и-dvb-eit)
- [CA / Newcamd / OSCam-mini](#ca--newcamd--oscam-mini)
- [Web UI и эксплуатация](#web-ui-и-эксплуатация)
- [Быстрая сборка из исходников](#быстрая-сборка-из-исходников)
- [Сборка на Ubuntu 24.04](#сборка-на-ubuntu-2404)
- [Сборка переносимого бинарника на Ubuntu 22.04](#сборка-переносимого-бинарника-на-ubuntu-2204)
- [Сборка на Debian / build-сервере](#сборка-на-debian--build-сервере-без-установки-gpu-драйверов)
- [Установка на обычную Linux-систему](#установка-на-обычную-linux-систему)
- [Создание portable-пакета](#создание-portable-пакета)
- [Docker](#docker)
- [Docker Compose](#docker-compose)
- [Обновление из Git](#обновление-из-git)
- [Тесты и диагностика](#тесты-и-диагностика)

---

## Основные возможности

### DVB и транспорт

- Linux DVB-S / DVB-S2.
- Сканирование адаптеров, транспондеров и сервисов.
- LOCK, уровень сигнала, качество и CNR.
- Несколько сервисов одного транспондера через общий DVB frontend.
- DiSEqC.
- Настраиваемые LNB LOF1/LOF2/SLOF.
- Unicable EN50494 и JESS/EN50607.
- DVB-S2 Stream ID / MIS.
- PID-фильтрация или полный transport stream.
- Remap SID/PID.
- Генерация PAT/PMT/SDT.
- Отдельные MPTS-выходы с выбранными сервисами.
- CBR pacing и целевой MPEG-TS bitrate.
- Основной и резервный input.
- Online и OnDemand режимы.

### IPTV

- UDP MPEG-TS.
- RTP MPEG-TS.
- HTTP/HTTPS MPEG-TS.
- HLS MPEG-TS.
- HLS fMP4/CMAF.
- SRT caller/listener.
- RTSP input/output.
- RTMP/RTMPS input/output.
- File input.
- VDR input.
- Встроенный test pattern.
- Дополнительные выходы для одного потока.

### Native transcoding

- H.264/AVC.
- H.265/HEVC.
- Copy mode.
- CPU codecs.
- NVIDIA NVENC.
- Intel QSV / oneVPL.
- Intel/generic VAAPI.
- NVIDIA NVDEC -> CUDA -> NVENC zero-copy, когда поддерживается системой.
- Для чересстрочного NVIDIA-видео обработка полей в NVDEC исправлена в V10.8.233; при проблемах с конкретным драйвером можно отключить GPU decode, сохранив NVENC encoding: `DVBSTREAMER5_DISABLE_NVIDIA_ZERO_COPY=1` в окружении сервиса.
- При `transcode_video_decoder=ittiam` или `openh264` выбранный CPU decoder имеет приоритет даже с `video_encoder=nvenc` (диагностический fallback).
- Экспериментальная A/B диагностика NVIDIA: `DVBSTREAMER5_NVDEC_SAFE_COPY=1` включает копирование NV12 между двумя CUDA device surfaces (без RAM/CPU), чтобы исключить проблему прямой регистрации NVDEC mapping в NVENC; по умолчанию отключена. `NATIVE NVDEC DECODE ERROR` показывает, что GPU сообщил о повреждённом входном кадре.
- Диагностика V10.8.233 для 1080i: `DVBSTREAMER5_NVDEC_VPP_MODE=adaptive|bob|weave` задаёт NVDEC VPP deinterlace (по умолчанию `adaptive`); `DVBSTREAMER5_NVDEC_VPP_PROBE=1` выводит флаги полей в журнал. При выключенном деинтерлейсе в настройках плитки используется `weave`, независимо от переменной. На уже работающих системах `bob` и `weave` следует проверять только в тестовом потоке, сравнивая артефакты и плавность движения.
- Дополнительный тест V10.8.233 для зелёных кадров: `DVBSTREAMER5_NVDEC_DIAG_CHROMA=neutral` принудительно включает GPU safe copy и заменяет только NV12 UV-плоскость на нейтральные значения U=V=128 **на GPU**. Изображение в этом режиме намеренно чёрно-белое; если цветные полосы исчезают, вероятно повреждена UV-плоскость. Режим не исправляет цвет и по умолчанию отключён; удалите переменную после диагностического теста.

- Исправление V10.8.233: планировщик `NativeTranscoderPipeline::muxWorkerLoop` больше не делает повторные короткие/просроченные `pthread_cond_clockwait`, когда один поток A/V превышает допустимое опережение в 500 мс. Вместо этого ожидает поступления данных от отстающего потока; для допустимого временного удержания ждёт точного срока. Ограничения 150/500 мс, логика сброса очередей и порядок mux остаются прежними. Это исправление нагрузки CPU, не исправление цветных полос NVIDIA; NVDEC/UV ещё требуют отдельной проверки.
- Диагностика V10.8.233: `DVBSTREAMER5_NVDEC_UV_PROBE=1` выводит статистику первых четырёх и каждого 500-го отображённого кадра: небольшие (не более 128 байт на область) пробы Y и двух UV-строк с CUDA device surface; с `DVBSTREAMER5_NVDEC_SAFE_COPY=1` также сверяет UV в целевом CUDA-буфере. Используйте вместе с `DVBSTREAMER5_NVDEC_DIAG_CHROMA` **отключённым**, иначе результат намеренно обесцвечен. Пробник включать временно: он создаёт редкие малые GPU→CPU чтения для диагностики и может слегка влиять на производительность.
- Native scaling.
- Deinterlace.
- Resolution, FPS, bitrate, GOP, B-frames, reference frames.
- Preset, profile, level и AQ.
- Выбор H.264 decoder для каждой плитки:
  - Auto;
  - Ittiam libavc;
  - OpenH264.
- Копирование/вставка настроек транскодирования между плитками.

### Audio

Native pipeline включает:

- AAC-LC через FDK-AAC;
- MPEG-1 Layer II / MP2;
- MP3 encode через LAME, если development package был доступен при сборке;
- AC-3 decode;
- audio copy для совместимых потоков.

### EPG

- DVB EIT ingest.
- External XMLTV.
- Несколько XMLTV-источников.
- Ручное обновление.
- Автоматическое daily/weekly обновление.
- Выбор XMLTV channel для каждой плитки.
- Название и описание передач.
- Режимы Auto / DVB / XMLTV.
- Опциональная генерация DVB EIT PID 0x12 в выходной MPEG-TS.
- Present/Following и schedule EIT.
- До 7 дней EPG для выходного EIT.

### DVB subtitles

- Опциональный passthrough DVB bitmap subtitles.
- Выбор языка.
- Выбор конкретного PMT subtitle track.
- Сохранение subtitle PES и PMT descriptors при транскодировании.

### Управление и мониторинг

- Встроенный Web UI.
- Русский и английский интерфейс.
- Плитки потоков и перезапуск отдельной плитки.
- Группировка DVB-плиток и отображение сигнала.
- Input/Output bitrate.
- CPU/GPU и runtime telemetry.
- Scheduled restart активных потоков.
- Telegram notifications.
- Subscriber/IP filtering.
- Опциональная фильтрация UDP/RTP ingress по выбранным интерфейсам.
- HLS access key через HTTP header или query parameter.
- HLS archive/timeshift.
- Multibitrate/ABR HLS.

---

## Поддерживаемые входы и выходы

| Протокол / источник | Input | Output |
|---|:---:|:---:|
| DVB-S/S2 | ✅ | — |
| UDP MPEG-TS | ✅ | ✅ |
| RTP MPEG-TS | ✅ | ✅ |
| HTTP/HTTPS MPEG-TS | ✅ | HTTP |
| HLS MPEG-TS | ✅ | ✅ |
| HLS CMAF/fMP4 | ✅ | ✅ |
| SRT | ✅ | ✅ |
| RTSP | ✅ | ✅ |
| RTMP/RTMPS | ✅ | ✅ |
| File | ✅ | — |
| VDR | ✅ | — |
| Test Pattern | ✅ | — |
| MPTS | source/remap | ✅ |

SRT используется из **vendored SRT 1.5.7**, который собирается статически с OpenSSL EVP.

---

## DVB-S/S2

Пример URI, который формирует Web UI:

```text
dvb://satellite?adapter=0&frontend=0&frequency_khz=12383000&symbol_rate=27500&polarity=H&delivery_system=dvb-s2&modulation=8psk&fec=3%2F4
```

Дополнительные DVB параметры:

```text
diseqc_source
lnb_lof1_khz
lnb_lof2_khz
lnb_slof_khz
unicable_mode
unicable_user_band
unicable_frequency_khz
unicable_position
unicable_pin
stream_id
pids
```

Поддерживаемые Unicable modes:

```text
none
en50494
en50607
```

---

## Транскодирование

### H.264 decoder

```text
auto
ittiam
openh264
```

### Video encoder

```text
auto
cpu
nvenc
qsv
vaapi
```

Hardware backend доступен только если:

1. нужные headers/libs были найдены во время CMake configure;
2. на runtime-сервере установлен совместимый driver;
3. GPU поддерживает выбранный codec/profile.

### Native codec sources

`scripts/vendor_native_codecs.sh` загружает pinned source snapshots:

- OpenH264;
- Ittiam libavc;
- libde265;
- Kvazaar;
- FDK-AAC;
- Ittiam libmpeg2;
- PL_MPEG.

Static prefix:

```text
third_party/native-codecs-prefix
```

Production-сборка не использует system FFmpeg/GStreamer codecs.

---

## HLS / CMAF / ABR / DVR

Поддерживаются:

- MPEG-TS HLS;
- fMP4/CMAF;
- segment duration;
- live segment count;
- AES-128;
- SAMPLE-AES;
- CMAF cbcs;
- `EXT-X-MAP`;
- `init.mp4`;
- `.m4s`;
- multibitrate/ABR HLS;
- HLS DVR/archive;
- timeshift;
- retention в часах;
- отдельный archive path.

Default archive path:

```text
/var/lib/dvbstreamer5/archive
```

---

## EPG и DVB EIT

EPG может использовать:

1. DVB EIT;
2. XMLTV;
3. Auto — DVB с fallback на XMLTV.

XMLTV source поддерживает:

```text
daily
weekly
```

При включении DVB EIT output приложение формирует EIT present/following и schedule для итогового output SID/TSID/ONID.

---

## CA / Newcamd / OSCam-mini

Проект собирает loadable CA backend:

```text
dvbstreamer5-ca-newcamd.so
```

Стандартный каталог:

```text
/opt/DVBStreamer5/ca-plugins
```

Также собирается vendored **OSCam-mini** для сценариев с локальным card-server/reader.

Стандартные пути:

```text
/opt/DVBStreamer5/oscam-mini/oscam-mini
/etc/dvbstreamer5/oscam-mini
```

OSCam-mini не требуется для обычных незашифрованных IPTV/DVB потоков.

---

## Web UI и эксплуатация

Default Web UI:

```text
port: 9000
login: admin
password: admin
```

После первой установки **обязательно смените пароль**.

При systemd deployment:

```text
/etc/dvbstreamer5/dvbstreamer5-config.json
/etc/dvbstreamer5/dvbstreamer5-subscribers.json
/etc/dvbstreamer5/dvbstreamer5-ui.key
```

Runtime layout:

```text
/opt/DVBStreamer5/bin/DVBStreamer5
/opt/DVBStreamer5/ca-plugins/
/opt/DVBStreamer5/oscam-mini/
/etc/dvbstreamer5/
/var/lib/dvbstreamer5/
```

---

# Сборка из исходников

## Быстрая сборка из исходников

```bash
git clone https://github.com/Monkipnet/DVBStreamer5.git
cd DVBStreamer5

./install_deps.sh

cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release

cmake --build build --parallel 2
```

Результат:

```text
build/DVBStreamer5
build/ca-plugins/dvbstreamer5-ca-newcamd.so
build/oscam-mini/oscam-mini
```

Проверка версии:

```bash
grep kProgramVersion src/AppVersion.h
```

---

## Сборка на Ubuntu 24.04

Рекомендуемый вариант для production-сервера.

```bash
sudo apt update
sudo apt install -y git

git clone https://github.com/Monkipnet/DVBStreamer5.git
cd DVBStreamer5

./install_deps.sh
```

`install_deps.sh` устанавливает build dependencies, подготавливает locked SRT/native codecs и пытается включить доступные hardware backends. На Ubuntu с NVIDIA скрипт может установить рекомендуемый driver.

После первой установки NVIDIA driver может потребоваться reboot:

```bash
sudo reboot
```

Сборка:

```bash
cd DVBStreamer5

rm -rf build

cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release

cmake --build build --parallel "$(nproc)"
```

На сервере с небольшим количеством RAM:

```bash
cmake --build build --parallel 2
```

Во время configure проверяйте нужные строки:

```text
Native Intel VAAPI backend: enabled
Native Intel QSV/VAAPI backend: enabled via oneVPL
Native NVIDIA NVENC backend: enabled
Native NVIDIA NVDEC->CUDA->NVENC zero-copy backend: enabled
```

---

## Сборка переносимого бинарника на Ubuntu 22.04

Если release должен запускаться на Ubuntu 22.04 и более новых совместимых Ubuntu, собирайте его на **самой старой целевой системе**.

Бинарник, собранный на Ubuntu 24.04, может требовать более новую glibc и не запускаться на Ubuntu 22.04.

Установка обязательных build packages:

```bash
sudo apt update

sudo apt install -y \
  git build-essential cmake pkg-config python3 nodejs \
  curl ca-certificates binutils nasm autoconf automake libtool \
  libpcsclite-dev libssl-dev libcrypt-dev libmp3lame-dev \
  libcurl4-openssl-dev libxml2-dev zlib1g-dev
```

VAAPI/oneVPL headers, если доступны:

```bash
sudo apt install -y libva-dev || true

if apt-cache show libvpl-dev >/dev/null 2>&1; then
  sudo apt install -y libvpl-dev
fi
```

Подготовка source tree:

```bash
git clone https://github.com/Monkipnet/DVBStreamer5.git
cd DVBStreamer5

./scripts/vendor_srt_source.sh
./scripts/vendor_native_codecs.sh

DVBSTREAMER5_CODEC_JOBS=2 \
  ./scripts/build_native_codecs.sh
```

Сборка:

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release

cmake --build build --parallel 2
```

Проверка:

```bash
./scripts/audit_runtime_deps.sh build/DVBStreamer5
ldd build/DVBStreamer5
```

### Важно

DVBStreamer5 не является полностью статическим Linux executable.

В проект статически входят SRT и native codec libraries, но остаются обычные system runtime dependencies: glibc, libstdc++, OpenSSL, libcurl, libxml2, zlib и optional hardware/audio runtimes.

Для максимальной совместимости:

> Собирайте release на самой старой системе, на которой он должен запускаться.

---

## Сборка на Debian / build-сервере без установки GPU-драйверов

Для отдельного build-server можно не запускать автоматическую установку GPU driver.

```bash
sudo apt update

sudo apt install -y \
  git build-essential cmake pkg-config python3 nodejs \
  curl ca-certificates binutils nasm autoconf automake libtool \
  libpcsclite-dev libssl-dev libcrypt-dev libmp3lame-dev \
  libcurl4-openssl-dev libxml2-dev zlib1g-dev

for p in libva-dev libvpl-dev; do
  if apt-cache show "$p" >/dev/null 2>&1; then
    sudo apt install -y "$p"
  fi
done
```

Затем:

```bash
git clone https://github.com/Monkipnet/DVBStreamer5.git
cd DVBStreamer5

./scripts/vendor_srt_source.sh
./scripts/vendor_native_codecs.sh

DVBSTREAMER5_CODEC_JOBS=2 \
  ./scripts/build_native_codecs.sh

cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release

cmake --build build --parallel 2
```

Если требуется только проверить hardware dependency setup без автоматической установки GPU driver:

```bash
sudo ./scripts/install_native_hw_backends.sh --no-driver-install
```

После изменения dev packages выполняйте clean configure:

```bash
rm -rf build

cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release

cmake --build build --parallel 2
```

---

## WSL / Windows workspace

Если source tree находится на `/mnt/c` или `/mnt/d`, CMake build tree лучше держать в Linux filesystem:

```bash
cmake -S /mnt/d/PROJECTS/DVBStreamer5 \
  -B "$HOME/DVBStreamer5-build" \
  -DCMAKE_BUILD_TYPE=Release

cmake --build "$HOME/DVBStreamer5-build" --parallel 2
```

---

# Установка

## Установка на обычную Linux-систему

После успешной Release-сборки:

```bash
sudo mkdir -p \
  /opt/DVBStreamer5 \
  /etc/dvbstreamer5 \
  /var/lib/dvbstreamer5/archive

sudo cmake --install build \
  --prefix /opt/DVBStreamer5 \
  --strip
```

Установить основной systemd unit:

```bash
sudo install -m 0644 \
  packaging/dvbstreamer5.service \
  /etc/systemd/system/dvbstreamer5.service

sudo systemctl daemon-reload
sudo systemctl enable --now dvbstreamer5.service
```

Проверка:

```bash
systemctl status dvbstreamer5.service --no-pager

journalctl -u dvbstreamer5.service \
  -n 100 -o cat --no-pager
```

Web UI:

```text
http://SERVER_IP:9000
```

### OSCam-mini

Если используется локальный OSCam-mini/card-reader:

```bash
sudo /opt/DVBStreamer5/oscam-mini/install_oscam_mini.sh

sudo systemctl status oscam-mini.service --no-pager
```

Не включайте OSCam-mini, если он не нужен.

---

## Переменные окружения

| Variable | Назначение |
|---|---|
| `DVBSTREAMER5_CONFIG_DIR` | каталог основной конфигурации |
| `DVBSTREAMER5_CA_PLUGIN_DIR` | каталог CA plugins |
| `DVBSTREAMER5_OSCAM_CONFIG_DIR` | каталог OSCam-mini configs |
| `DVBSTREAMER5_OSCAM_BINARY` | путь к OSCam-mini executable |

Systemd unit использует:

```text
DVBSTREAMER5_CONFIG_DIR=/etc/dvbstreamer5
DVBSTREAMER5_CA_PLUGIN_DIR=/opt/DVBStreamer5/ca-plugins
DVBSTREAMER5_OSCAM_CONFIG_DIR=/etc/dvbstreamer5/oscam-mini
DVBSTREAMER5_OSCAM_BINARY=/opt/DVBStreamer5/oscam-mini/oscam-mini
```

---

# Portable package

## Создание portable-пакета

После Release-сборки:

```bash
VERSION=$(sed -n 's/.*kProgramVersion = "\([^"]*\)".*/\1/p' src/AppVersion.h)
ARCH=$(uname -m)

STAGE="$PWD/dist/rootfs"
OUT="$PWD/dist/DVBStreamer5-${VERSION}-linux-${ARCH}.tar.gz"

rm -rf "$STAGE"
mkdir -p "$STAGE"

DESTDIR="$STAGE" \
  cmake --install build \
  --prefix /opt/DVBStreamer5 \
  --strip

install -Dm644 \
  packaging/dvbstreamer5.service \
  "$STAGE/etc/systemd/system/dvbstreamer5.service"

mkdir -p \
  "$STAGE/etc/dvbstreamer5" \
  "$STAGE/var/lib/dvbstreamer5/archive"

tar -C "$STAGE" -czf "$OUT" .

echo "$OUT"
```

Проверка пакета:

```bash
tar -tzf "$OUT" | head -n 100

./scripts/audit_runtime_deps.sh build/DVBStreamer5
ldd build/DVBStreamer5
```

### Установка portable package

```bash
sudo tar -xzf DVBStreamer5-*-linux-*.tar.gz -C /

sudo mkdir -p \
  /etc/dvbstreamer5 \
  /var/lib/dvbstreamer5/archive

sudo systemctl daemon-reload
sudo systemctl enable --now dvbstreamer5.service
```

Проверка missing libraries:

```bash
ldd /opt/DVBStreamer5/bin/DVBStreamer5 |
grep 'not found'
```

---

# Docker

Обновлённый Dockerfile использует Ubuntu 24.04 и самостоятельно:

- ставит build dependencies;
- загружает locked SRT;
- загружает pinned codec sources;
- собирает native codec prefix;
- собирает DVBStreamer5;
- собирает Newcamd plugin;
- собирает OSCam-mini;
- создаёт отдельный runtime image.

## Docker build

```bash
git clone https://github.com/Monkipnet/DVBStreamer5.git
cd DVBStreamer5

docker build \
  --build-arg CODEC_JOBS=2 \
  --build-arg BUILD_JOBS=2 \
  -t dvbstreamer5:10.8.233 .
```

Persistent directories:

```bash
mkdir -p docker/config docker/archive
```

## IPTV-only container

На Linux для multicast UDP/RTP/SRT рекомендуется host network:

```bash
docker run -d \
  --name dvbstreamer5 \
  --restart unless-stopped \
  --network host \
  -v "$PWD/docker/config:/etc/dvbstreamer5" \
  -v "$PWD/docker/archive:/var/lib/dvbstreamer5" \
  dvbstreamer5:10.8.233
```

## Docker + DVB adapter

Пример adapter0:

```bash
docker run -d \
  --name dvbstreamer5 \
  --restart unless-stopped \
  --network host \
  --device /dev/dvb/adapter0/frontend0 \
  --device /dev/dvb/adapter0/demux0 \
  --device /dev/dvb/adapter0/dvr0 \
  -v "$PWD/docker/config:/etc/dvbstreamer5" \
  -v "$PWD/docker/archive:/var/lib/dvbstreamer5" \
  dvbstreamer5:10.8.233
```

Посмотреть device nodes:

```bash
find /dev/dvb -maxdepth 3 -type c -ls
```

Если конкретный драйвер создаёт дополнительные `ca*`, `net*` или `demux*`, передайте и их.

## Docker + Intel VAAPI/QSV

Проверка host:

```bash
vainfo --display drm --device /dev/dri/renderD128
```

Запуск:

```bash
docker run -d \
  --name dvbstreamer5 \
  --restart unless-stopped \
  --network host \
  --device /dev/dri/renderD128 \
  --device /dev/dri/card0 \
  -v "$PWD/docker/config:/etc/dvbstreamer5" \
  -v "$PWD/docker/archive:/var/lib/dvbstreamer5" \
  dvbstreamer5:10.8.233
```

## Docker + NVIDIA NVENC

На host нужны proprietary NVIDIA driver и NVIDIA Container Toolkit.

```bash
nvidia-smi
```

Проверка Docker GPU:

```bash
docker run --rm --gpus all \
  nvidia/cuda:12.6.0-base-ubuntu24.04 \
  nvidia-smi
```

Запуск:

```bash
docker run -d \
  --name dvbstreamer5 \
  --restart unless-stopped \
  --network host \
  --gpus all \
  -e NVIDIA_DRIVER_CAPABILITIES=video,compute,utility \
  -v "$PWD/docker/config:/etc/dvbstreamer5" \
  -v "$PWD/docker/archive:/var/lib/dvbstreamer5" \
  dvbstreamer5:10.8.233
```

DVB + NVIDIA: добавьте к этой команде нужные `--device /dev/dvb/...`.

---

## Docker Compose

В репозитории есть `docker-compose.yml`.

```bash
mkdir -p docker/config docker/archive

docker compose up -d --build
```

Логи:

```bash
docker compose logs -f dvbstreamer5
```

Остановка:

```bash
docker compose down
```

Для DVB/Intel/NVIDIA включите нужные device/GPU options в compose-файле.

---

# Обновление из Git

```bash
cd /path/to/DVBStreamer5

git switch main
git fetch origin
git pull --ff-only origin main
```

После обновления:

```bash
rm -rf build

cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release

cmake --build build --parallel 2
```

Переустановка:

```bash
sudo systemctl stop dvbstreamer5.service

sudo cmake --install build \
  --prefix /opt/DVBStreamer5 \
  --strip

sudo install -m 0644 \
  packaging/dvbstreamer5.service \
  /etc/systemd/system/dvbstreamer5.service

sudo systemctl daemon-reload
sudo systemctl start dvbstreamer5.service
```

`/etc/dvbstreamer5` при этом не удаляется.

---

# Тесты и диагностика

## Native tests

```bash
cmake -S . -B build-tests \
  -DCMAKE_BUILD_TYPE=Release \
  -DDVBSTREAMER5_BUILD_MEDIA_CORE_TESTS=ON \
  -DDVBSTREAMER5_BUILD_MP2_ENCODER_TESTS=ON

cmake --build build-tests --parallel 2

ctest \
  --test-dir build-tests \
  --output-on-failure
```

## Runtime dependency audit

```bash
./scripts/audit_runtime_deps.sh build/DVBStreamer5
```

Audit проверяет отсутствие случайной runtime dependency на FFmpeg/libav, GStreamer, external libsrt и system codec libraries.

## Service

```bash
systemctl status dvbstreamer5.service --no-pager

journalctl -u dvbstreamer5.service \
  -f -o cat
```

## DVB

```bash
find /dev/dvb -maxdepth 3 -type c -ls
```

## NVIDIA

```bash
nvidia-smi
```

## Intel VAAPI

```bash
vainfo --display drm --device /dev/dri/renderD128
```

## oneVPL

```bash
vpl-inspect
```

## Dynamic libraries

```bash
ldd /opt/DVBStreamer5/bin/DVBStreamer5
```

---

# Архитектура native media engine

Production media path включает собственные компоненты DVBStreamer5:

- MPEG-TS framing;
- PAT/PMT/SDT;
- PES;
- PTS/DTS/PCR;
- remap;
- CBR pacing;
- HLS segmenter;
- CMAF;
- SAMPLE-AES;
- SRT;
- RTP;
- RTSP;
- RTMP;
- native transcoder;
- hardware codec backends;
- EPG/DVB EIT;
- DVB subtitle passthrough;
- CA plugin interface.

Дополнительная техническая информация:

```text
NATIVE_MEDIA_ENGINE.md
third_party/NATIVE_CODEC_SOURCE_LOCK.txt
```

---

## Рекомендации для deployment

- **Обычная systemd installation** — лучший вариант для DVB PCIe/USB, PC/SC readers и прямого hardware access.
- **Docker** — удобен для IPTV/SRT/HLS и контейнерной инфраструктуры; DVB/GPU devices нужно явно передавать контейнеру.
- **Portable package с Ubuntu 22.04 build host** — удобен для переноса одного release между несколькими совместимыми Ubuntu-серверами.

Перед обновлением production headend рекомендуется проверить новый release на отдельном сервере или тестовом потоке.
