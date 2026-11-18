# XMLTV/EPG memory optimization (version stays 10.8.580)

The streaming XMLTV parser continues scanning provider listings and exposes the full channel-name catalog for the channel picker. It retains programme objects in process memory only for EPG-enabled stream configurations with a selected XMLTV channel ID (including auto EPG fallback). This minimizes steady-state memory when a guide advertises thousands of channels but a few are used. Historical parser limits and per-channel retention remain unchanged.

The compressed disk cache stores the selected programme subset and full channel-name metadata. At startup, only required events are restored. When the configured channel selection changes, a new download is queued to populate newly required EPG entries; until then they may be unavailable. No media pipeline changes are involved.

Reconfiguring sources now moves existing per-source EPG rather than deep-copying all programme descriptions. Completion of an in-flight refresh filters out any EPG entries no longer selected and requeues if selection changed during the download.

This improvement does not prove that XMLTV accounted for the previously observed ~1.7 GiB of process RSS. Snapshot and platform memory measurements are still necessary. Tests verify retention and refresh logic via static source assertions; full replay testing with a large provider feed remains to be performed.
