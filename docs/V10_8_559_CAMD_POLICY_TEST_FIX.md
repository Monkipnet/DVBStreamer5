# DVBStreamer5 V10.8.559 — correct synthetic cold-start timestamps in recovery test

Self-review caught an invalid V558 test case: it asserted that a never-clear
TSDecrypt child should not recover after 189 seconds of CAMD blackout, although
the explicit policy correctly requires recovery after 180 seconds. The test
now checks no restart at 169 seconds, and recovery at 181 seconds; V558's
faster 90-second post-success stall recovery and shared per-reader 30-second
restart gate are unchanged. CI should validate this with full CTest and media
regression and produce a signed/checksummed portable archive.
