# V10.8.503 — inline browser preview proof wiring

The V502 control panel added a browser-local video-frame evidence listener,
but only the separately served development copy
`web/preview/preview-player.js` dispatched that event.
The real bundled Control Panel includes its preview player inline inside
`src/http/HttpServerWebUi.inc`. As a result V502 could show "ДЕКОД: PES"
but could not transition to "ДЕКОД: ОК" based on a presented preview frame.

V503 updates the embedded player to emit the same signal and makes CI test
both copies. No decoder, CAMD, CA transport, streaming protocol or preview
playback path changes. Playback evidence is browser-local and expires after
15 seconds without newly presented frames.

This is a UI correctness fix; it does not claim recovery of card-denied
events or missing control words.
