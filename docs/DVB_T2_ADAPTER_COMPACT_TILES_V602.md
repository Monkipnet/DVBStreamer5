# DVBStreamer5 V10.8.602 — DVB-T/T2 adapter edit + Astra/Cesbo compact view

> One version for both features. The existing full-size tiles remain the
> installation default. No live DVB, network or CA services are modified by
> switching the tile layout.

## Main dashboard — edit DVB-T/T2 adapters

1. Locate the DVB-T/T2 **physical adapter card** in the main dashboard.
2. Click the **adapter heading** to edit it. With one multiplex, its editor
   is selected directly. With several configured frequencies or PLPs,
   choose the required MUX first; **no arbitrary frontend is selected**.
3. Expand the adapter card to see one group per physical multiplex,
   identified by adapter/frontend, DVB-T or DVB-T2, RF frequency, bandwidth,
   and DVB-T2 PLP. Click the **MUX title** for the same edit action. The
   large spoiler/clickable empty space only expands/collapses the tiles.
4. A prompt identifies the exact configured channels that will be stopped
   before editing. Cancel does not stop or alter anything. A busy frontend
   belonging to another live group is rejected, not forcibly stopped.
5. After confirmation, the UI waits until related channels finish stopping,
   verifies frontend ownership, then opens the shared DVB editor with
   **DVB-T/T2, adapter, frontend, frequency, bandwidth and PLP already set**.
   No probes are issued to the default/wrong adapter while preparing.
6. Save tuning to **all services on the selected MUX** using the existing
   `/api/dvb-transponder/edit` path. The server verifies no consumer is
   active, exact original URI identity, and valid RF/PLP/bandwidth.
   Service-specific SID, PNR, PID filters, jitter, CA, outputs and custom
   URI keys are preserved. Channels remain stopped until explicitly started.

The satellite DVB-S/S2 transponder editor and group controls are unchanged.

## Optional compact tile view (Astra / Cesbo style)

Go to **Menu → Administration → Tile display → Compact Astra / Cesbo view**.

* Compact 60px-high tiles with channel name and live output bitrate.
* Green: actual outgoing packets/bitrate; red: running but no observed
  outgoing bitrate; muted grey: stopped. Stop pending is highlighted.
* `⋯` launches the same existing stream controls (Start/Stop including
  force stop, Edit, Quality graph, Copy URL(s), Delete with confirmation).
* Menus are outside clipped tiles, support keyboard arrows, Home, End,
  Escape (returns focus), click-away and viewport/scroll closing.
* WISI EPG virtual tiles have separate control menus and never become
  media stream actions.
* Nested DVB-T/T2 MUX headings take the full width of the physical-card
  spoiler in **both** standard and compact modes, rather than becoming
  narrow channel-sized cards.
* Existing EPG hover, double-click preview, tile search, status updates,
  physical-group spoilers and per-field visibility remain available.
* The visual layout is a per-browser localStorage preference, not a server
  or stream configuration parameter. Clean installs still use the original
  tile design.

All **new** labels and prompts support RU, EN, HR and DE. The 17 tile
visibility fields now have German labels as well.

## Tests

`tests/web/test_terrestrial_adapter_edit_v602.js`: full dashboard JS syntax,
exact MUX grouping with same-RF/different-PLP fixtures, valid/invalid
frequency/bandwidth, frontend tuning payload, group selection, server API
guards, compact-mode coexistence.

`tests/test_dvb_terrestrial_mux_edit_v602.cpp`: independent DVB-T/T2
channels retain SID/PNR, PID filters, jitter and vendor options while the
chosen group's frequency, bandwidth, modulation and PLP are updated.

`tests/web/test_astra_cesbo_compact_tiles_v601.js`: default original mode,
compact menu actions, WISI EPG separation, keyboard navigation, zero-bitrate
alert and four-language settings. This test is also run for **V602**.

The GitHub Actions workflow
`.github/workflows/build-main-linux.yml` executes all three checks without
physical tuners, as well as existing CTest/native build steps. Passing source
static/emulated tests is **not** the same as production verification. Check
the actual Actions run and test on the nonproduction `home2` server before
deploying to a production host.

## Build and installation

Build and package from the **source root**. Standard portable archives include
OSCam-mini and iCAM TSDecrypt. Installation of a new binary can stop/restart
streams; changing layout alone cannot.
