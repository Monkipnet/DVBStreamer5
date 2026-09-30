# Stage 6 — Embedded SRT runtime

- removes external `libsrt*.so` lookup from NativeSrtTransport
- embeds an SRT 1.5 ELF payload into DVBStreamer5 at link time using objcopy
- loads the embedded payload from anonymous Linux memfd
- binary installer no longer installs libsrt runtime packages
- source builds use an SRT 1.5 shared object only as a build-time payload; CMake copies its bytes into the final ELF
- optional `scripts/vendor_srt_runtime.sh` can snapshot a target-compatible SRT 1.5 payload into `third_party/srt-embedded/` for offline/reproducible builds
- runtime audit rejects DT_NEEDED entries for both GStreamer and SRT

This stage intentionally embeds the distro-compatible SRT 1.5 payload selected on the build host instead of hardcoding a binary built against a newer glibc. This preserves Ubuntu 22.04/24.04 compatibility when the application is built on the target baseline.
