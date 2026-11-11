# DVBStreamer5 V10.8.465 — authoritative tile Stop and uptime

## Regression

In V454, /api/stop-stream queued a nonblocking teardown and returned
success before the underlying pipeline stopped. The web UI immediately
forced active=false and reset running_seconds, but later polling displayed
the unchanged, still-live generation. OnDemand clients and automatic
failover could also race manual Stop.

## New behavior

- A Stop intent is remembered for the current process lifetime, even
  when a channel is already offline.
- Automatic start paths, including OnDemand and source failover, cannot
  override an operator Stop or an asynchronous teardown in progress.
- The stop worker targets the current generation. Explicit Start while
  a queued Stop is pending is rejected with a useful error.
- API state includes stop_pending; UI displays Stopping... and disables
  the button until the backend finishes. Uptime is not artificially
  cleared while the stream is still alive.
- Once physically stopped, the UI reports offline; a new manual Start
  clears the gate and creates a fresh generation with a reset timer.

Other running streams and CA backends are unaffected. The stop intent
is runtime-only; normal startup settings apply after a daemon restart.

## Verification

From the repository root:

    git switch main
    git pull --ff-only origin main
    cmake -S . -B build
    cmake --build build -j"$(nproc)"
    ctest --test-dir build --output-on-failure
    node tests/web/test_stream_toggle_stop_policy.js

On a TEST tile, Stop must first show Stopping... then Offline/Start.
The timer should reset only when the backend actually stops; explicit
Start must create a new StreamState with a new monotonic start generation.
