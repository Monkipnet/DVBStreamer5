# Stage 8 — SRT 1.5.7 built from vendored source

- Removes the build-time dependency on any installed `libsrt1.5-*` or `libsrt*-dev` package.
- Removes the Stage 6/7 embedded shared-object payload, `memfd`/`dlopen` loader and GnuTLS/Nettle ABI shims.
- Locks Haivision SRT to version 1.5.7 / release commit `899348d8318eb9a3c5a5b6ec43c4a1114288773a`.
- Builds upstream `srt_static` directly as a CMake subproject from `third_party/srt`.
- Builds SRT with encryption enabled and `USE_ENCLIB=openssl-evp`; shared SRT, apps, examples and upstream tests are disabled.
- `NativeSrtTransport` calls the linked SRT C API directly; there is no runtime SRT loader.
- Native SRT regression test uses an encrypted caller/listener loopback and requires runtime version >= 1.5.7.
- The runtime audit rejects external `libsrt`, GStreamer, GnuTLS and Nettle dependencies.

`./scripts/vendor_srt_source.sh` populates `third_party/srt` from the pinned SRT 1.5.7 orig source archive and verifies SHA-256 before extraction. Once populated, the source tree is intended to be committed to the DVBStreamer5 repository so subsequent builds are network-independent.
