# DVBStreamer5 V10.8.553 — TLS-aware cluster routing regression

The V537 static regression expected `validOrigin(peer.viewerUrl)` while the
current cluster security policy validates the remote viewer origin using
`validOrigin(peer.viewerUrl,!peer.verifyTlsPeer)`. V553 updates only the
static assertion to check the *current* TLS-aware behavior. It does not
remove hostname/origin verification and does not modify a cluster endpoint.

The V552 runtime-recovery, PCR and bounded systemd stop changes remain intact.
The Linux portable CI and CodeQL/OSV audit must be interpreted separately;
the OSV scan flags CVE-2025-26803 in vendored Boost and requires dependency
triage rather than silently suppressing the finding.
