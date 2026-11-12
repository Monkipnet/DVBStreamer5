# DVBStreamer5 V10.8.507 — authenticated admin preview from any IP

## Observed regression
The authenticated /api/streams/<id>/preview manifest preferred normal
public /stream/<id>.ts for channels with configured HTTP, and
/<channel>/video.m3u8 for channels with configured HLS. Those addresses
are governed by IPTV subscriber IP allowlists and stream middleware.
A logged-in administrator connecting from a different IP thus saw 403.

## Fix
- The admin manifest always supplies the protected private
  /api/streams/<id>/preview.ts route, even if ordinary HTTP is enabled.
- For configured HLS, the admin HLS playlist is served at
  /api/streams/<id>/preview.hls/video.m3u8, with its relative media
  segments and init maps served through the same authenticated prefix.
- These resources reuse existing HLS disk storage, live channel pipeline,
  existing playlist player and native browser preview hub.
- Each admin HLS path is resolved by validated **exact stream ID**, not
  collision-prone public channel title.
- A panel session cookie or valid admin Basic auth is required for every
  /api/ request, including every segment. Logout revokes this access.
  No direct public IPTV URL is exempted from IP filtering or stream auth.
- No new public output, input retune, transcoding settings or CA changes.

## Tests
node tests/test_admin_preview_any_ip.js
node tests/test_preview_hls_20372.js
node tests/test_preview_close_20368.js

The client still needs network access to the administrative HTTP(S) port;
a firewall or NAT restriction cannot be overridden by application login.
Browser codec support may also limit actual decoding.
