# DVBStreamer5 V10.8.506 — stable TSDecrypt PES/OK indication

The V505 runtime and V504 user interface required a *browser preview* with
video-frame callbacks to display green `ДЕКОД: ОК`. A normal HTTP/HLS/SRT
viewer could watch a working channel while the control panel stayed
`ДЕКОД: PES`; closing the preview returned green to PES.

V506 adds a server-side **UI-only** per-stream evidence gate. A known active
stream client session together with two distinct, positive clear-PES output
samples can promote the stream to `viewer_pes_ok`, displayed as green
`ДЕКОД: ОК`. The state remains green after client disconnection **as long
as PES output continues**. A brief 1–6 second gap is tolerated to avoid
flicker. After longer PES starvation, loss of output, stopping or restarting
the stream, the state resets; a viewer connection alone never turns green.

The label is operational TS+viewer confirmation, not independent full-frame
codec validation. Browser-presented video remains the strongest evidence;
the UI title differentiates the two. This policy does not change CW, ECM,
TS packet flags, transcoding, authentication, subscriber access, CA clients,
existing delivery, or DVB frontend ownership detection from V505.

CI also fixes the version-specific UI test that caused the V505 build to
stop before compilation. The test now recognizes subsequent 10.8 versions.
