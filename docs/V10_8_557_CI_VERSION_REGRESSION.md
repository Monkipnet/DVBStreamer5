# DVBStreamer5 V10.8.557 — future-proof V552 regression and test multi-SID CAMD recovery

The first V556 GitHub Actions run stopped before compiling because
`tests/web/test_runtime_recovery_v552.js` asserted an exact, stale
`10.8.555` release number. V557 retains all V556 runtime improvements,
but replaces that brittle equality with a check for a valid version >= V552.
It also exercises synthetic five-SID deterministic staggered CAMD blackout
recovery, including channels from the home2 incident. CI must still pass
full CMake, CTest and portable TSDecrypt rebuild. This change does not
alter the ECM/reader timeout or the external provider.
