# Embedded SRT runtime payload

Stage 6 embeds an SRT 1.5 shared-object payload into the final `DVBStreamer5`
ELF at build time. The payload is converted into a read-only ELF object by
`objcopy`; at runtime `NativeSrtTransport` writes those bytes to an anonymous
Linux `memfd` and loads `/proc/self/fd/<n>` with `dlopen()`.

There is no `DT_NEEDED` entry for `libsrt*`, no `libsrt*.so` beside the
application, and the binary installer does not install a separate SRT runtime.

For source builds, provide a compatible Linux x86_64 SRT 1.5 runtime as the
embedding input. `./install_deps.sh` installs one and
`./scripts/vendor_srt_runtime.sh` can copy it into this directory as
`libsrt-runtime.so`. You may also configure CMake with:

    -DDVBSTREAMER5_SRT_RUNTIME=/absolute/path/to/libsrt-gnutls.so.1.5

Upstream: https://github.com/Haivision/srt
License: Mozilla Public License 2.0. See `LICENSE-MPL-2.0.txt`.

For redistribution, keep the exact upstream/source offer appropriate to the
SRT build you embed. The helper writes `RUNTIME_INFO.txt` with package/version
and SHA-256 when available.
