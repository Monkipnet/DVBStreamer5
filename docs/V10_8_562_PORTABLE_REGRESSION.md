# V10.8.562 — preserve legacy portable installer item ordering

V561 adds the Python 3.6-safe ECM latency analyzer to bundled
`scripts/`. The existing SSL lifecycle regression asserts that the
installer's ordered item list contains `bin lib web ca-plugins etc tls
run.sh`. Inserting `scripts` between `tls` and `run.sh` caused the
prebuild test to fail, despite not affecting TLS runtime behavior.

V562 retains the original item ordering and appends `scripts` last.
No ECM telemetry or binary logic changes since V560. GitHub Actions must
pass integrated SSL regression, compile oscam-mini, run CTest and produce
the portable package before deployment.
