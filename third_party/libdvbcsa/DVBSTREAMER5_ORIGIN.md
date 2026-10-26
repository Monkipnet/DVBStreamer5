# Vendored libdvbcsa

Source: official VideoLAN repository `https://code.videolan.org/videolan/libdvbcsa.git`.

Imported commit: `c57607040ec007c173a5de1dc870b46d013154c1`.
Upstream version: `1.1.0`.
License: GPL-2.0-or-later; see `COPYING`.

DVBStreamer5 builds the SSE2 bitslice implementation because the supported target is
Linux x86_64. Unused MMX, ARM/NEON, AltiVec and scalar transpose variants are omitted.
The allocation shim in `dvbcsa_bs_algo.c` also supports MSVC `_aligned_malloc`
so the vendored target can be compile-checked in the Windows development environment.


## V10.8.186 iCAM extension

DVBStreamer5 keeps the normal DVB-CSA API and adds the ECM-aware VideoGuard/iCAM
key-schedule entry points used by the native Newcamd backend:

- `dvbcsa_bs_key_set_ecm()`
- `dvbcsa_key_set_ecm()`

The ECM-aware block-key schedule follows the public libdvbcsa iCAM extension used
by current receiver/softcam integrations. Ordinary CAIDs continue to use the
standard `dvbcsa_key_set()` / `dvbcsa_bs_key_set()` path.
