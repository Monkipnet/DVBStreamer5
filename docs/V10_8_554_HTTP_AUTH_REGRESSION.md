# DVBStreamer5 V10.8.554 — preserve the HTTP TS client-IP authorization regression

An older V537 source-text test searched for `handleHttpStream(socket, target)`.
The implemented current HTTP handler passes a third argument,
`clientIpForRequest(socket,req)`, to enforce client/subscriber authorization.
The assertion now checks the current three-argument media entrypoint rather
than incorrectly failing on this security-relevant change. No production
media or authentication behavior was modified; the complete V552 recovery
and bounded systemd termination changes remain included.
