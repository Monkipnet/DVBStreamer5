# DVBStreamer5 V10.8.601 — optional Astra / Cesbo compact tile layout

## Purpose

The screenshot-based Astra/Cesbo-style layout is a **second, optional view**
of DVBStreamer5's existing dashboard. No media, stream, CA, HTTP, HLS or
configuration API changes were made.

**A new installation and any browser without a saved preference continue using
the original full-size tile view.** Changing the view does not Start/Stop,
restart, or modify streams.

## How to enable

1. Open **Menu → Administration → Tile display**.
2. Under **Tile layout** choose **Compact Astra / Cesbo view**.
3. Close the settings dialog. The live dashboard is immediately laid out as
   multiple small tiles (~145 px minimum width, ~60 px height), preserving
   DVB adapter/transponder and network-group organization.
4. Restore **Standard (default)** at any time from the same dialog.

The selection persists locally per browser via
`localStorage['dvbstreamer5-tile-layout-v601']`, independently of visible tile
fields and the dark/light theme. It does not alter the server config or other
users' preferences. Existing tile field visibility checkboxes still govern
the standard view.

## Compact stream tiles

* Display the channel name and its **live outgoing bitrate in kbit/s**.
* Active output with bitrate → green. Running with no observed output → red.
  Stopped/OnDemand → gray "Inactive". Stop pending → amber.
* One `⋯` control opens a separate floating menu outside the clipped tile:
  Start/Stop (including force-stop pending logic), Edit, Quality chart,
  Copy URL, Delete. The menu uses the same existing dashboard handlers,
  including the existing delete confirmation.
* The popup is positioned within the viewport, opens by click or keyboard,
  closes with Escape, by clicking outside, on scrolling/resizing, or when the
  tiles are re-rendered. The EPG hover is hidden while it is open.
* The original double-click preview behavior remains available when clicking
  the tile outside its control.

## WISI EPG tiles

WISI EPG outputs use the same compact layout and a dedicated `⋯` menu
(Start/Stop, Edit, Enable/Disable autostart). Their actions continue to use
`/api/epg-udp/action` and **are never treated as media stream actions**.

## Languages and test coverage

All added setting names, menu actions and statuses support RU, EN, HR and DE.
The current full-size dashboard and its controls are not removed or changed
by a fresh installation.

`tests/web/test_astra_cesbo_compact_tiles_v601.js` verifies:

* Entire web UI JavaScript parses correctly
* Standard default + persistent compact selection and settings dialog labels
* Compact CSS and no clipped/hidden menu actions
* Simulated floating menu actions on stream and WISI EPG virtual tiles
* Popup viewport positioning and expanded/closed state

The test runs from `.github/workflows/build-main-linux.yml` without DVB
hardware. Real browser rendering and production behavior must still be
checked separately from CI.
