# Stage 7 — SRT crypto on OpenSSL

- Keeps the SRT 1.5 transport embedded in `DVBStreamer5`.
- Adds embedded `libgnutls.so.30` and `libnettle.so.8` compatibility SONAMEs backed only by OpenSSL.
- Implements the seven crypto ABI functions used by SRT 1.5.x: `gnutls_rnd`, AES key setup/encrypt/decrypt, CTR and PBKDF2-HMAC-SHA1.
- Loads both shims from `memfd` with `RTLD_GLOBAL` before the SRT payload.
- Binary installer no longer installs GnuTLS or Nettle.
- Native SRT test now uses encrypted caller/listener traffic and fails if system `libgnutls.so` or `libnettle.so` appears in `/proc/self/maps`.
- Main ELF remains free of `DT_NEEDED` entries for GStreamer, SRT, GnuTLS and Nettle.
