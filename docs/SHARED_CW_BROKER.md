# Shared ECM/CW broker — V10.8.472 (phase 1)

This release introduces an **opt-in** exact-match CW response broker for the
native Newcamd CA plugin. Production defaults remain unchanged.

## Separation of responsibilities

- OSCam-mini already maintains an ECM/CW cache at the reader/server level
  (the same OSCam can answer both DVBStreamer5 native Newcamd sessions and
  the external TSDecrypt child processes). The reader is the authoritative
  source. Do not treat a CW from another service as interchangeable.
- DVBStreamer5 adds an in-process, reader-scoped **response reuse cache** for
  native Newcamd only. It cannot yet directly supply CW to the isolated
  external TSDecrypt binary, which has its own Newcamd socket and CSA state.
  A controlled IPC adapter is a separate project stage, **not implemented**.

## Boundaries and safety

Native cache keys contain the exact ECM section signature and the reader
identity, service ID, CAID, provider, ECM PID, and iCAM ECM mode.
All fields must match before the key can be reused. This **does not**
share CW between SID 105, 108 and 132, which use different ECMs.
It may help when two tiles receive the same service and exact ECM.

- Cache is private to the Newcamd plugin process; no network listener,
  external key-export endpoint, persisted files, or CW logging is added.
- Entries expire at 1,500 ms, with at most 128 entries total per backend.
  Even/odd keys are stored and replayed together, never as half a CW.
- Zero/partial results and negative Newcamd responses are never cached.
  Reader closure invalidates its entries. Entry buffers are explicitly wiped.
- Cached replies follow the *existing* Newcamd callback path (including
  FastECM/iCAM parity staging); a separate delivery thread avoids calling
  back while the TS packet mutex is held.
- Original per-service TCP sessions and EMM/AU flow are unchanged.
- This phase reuses completed CW responses. **In-flight ECM coalescing
  across independent sessions is not implemented yet.**
- Concurrent access must be verified by actual integration testing before
  enabling on busy production servers.

## Enable on a test instance (disabled by default)

Set environment variable `DVBSTREAMER5_SHARED_CW_BROKER=1` in the
test instance's systemd environment, then restart that instance only
during a maintenance window. Removing the variable and restarting
returns to the unchanged pre-V472 path.

Do not set this in the production unit until regression tests of Newcamd,
iCAM parity behavior, channel stop/start, and EMM authorization pass.

## Diagnostics

The native Newcamd backend status JSON exposes:

- `shared_cw_broker_enabled`
- `shared_cw_cache_hits`
- `shared_cw_cache_misses`
- `shared_cw_cache_stores`
- `shared_cw_cache_expired`
- `shared_cw_cache_entries`

Only counters are exposed, never CW bytes or ECM signatures.

Build and test from the repository root:

```bash
cmake -S . -B build
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
ctest --test-dir build -R shared_cw_broker --output-on-failure
```

Do not expect this cache alone to cure OSCam's `card_denied` on
SID 132 or permit nine distinct channels to decrypt. A reply with
no CW cannot be corrected by recycling another service's CW.

## Next phase

For external TSDecrypt, consider an authenticated local-only IPC interface
owned by the CA backend, with ephemeral authorization tokens, per-service
ECM provenance, strictly bounded queues, full CW zeroization, and
backward-compatible fail-closed fallback. None of this protocol is active
in V472; it requires a separate reviewed implementation and load test.
