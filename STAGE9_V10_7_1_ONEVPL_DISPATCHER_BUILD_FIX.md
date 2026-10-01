# Stage 9 V10.7.1 — oneVPL dispatcher build fix

V10.7 used the oneVPL 2.x dispatcher entry points (`MFXLoad`, `MFXCreateConfig`,
`MFXSetConfigFilterProperty`, `MFXCreateSession`, `MFXUnload`) but included only
`<vpl/mfxvideo.h>`. Those dispatcher types/functions are declared by
`<vpl/mfxdispatcher.h>`.

V10.7.1 fixes the native Intel QSV/VAAPI build path by:

- including `<vpl/mfxdispatcher.h>` before `<vpl/mfxvideo.h>`;
- changing CMake detection to require the dispatcher header, not merely
  `mfxvideo.h`;
- still requiring `libvpl` and `mfxvideo.h` before enabling
  `DVBSTREAMER5_HAVE_VPL`.

No FFmpeg, libav or GStreamer dependency is added. NVENC and CPU codec paths are
unchanged.
