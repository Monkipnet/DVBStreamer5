# DVBStreamer5 V10.8.508

V507 introduced authenticated, private HTTP/HLS preview URLs but its CI
test incorrectly looked for requiresAuthentication() in the common-helpers
include. The implementation lives in HttpServerTransport.inc, where every
/api/ request is checked before preview HLS/TS routes.

V508 fixes the regression test so CI exercises the actual source contract.
The V507 private preview functionality and public IPTV authorization rules
are unchanged.
