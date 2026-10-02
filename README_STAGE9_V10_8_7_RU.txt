DVBStreamer5 Stage 9 V10.8.7 - Hardware backend auto setup

CMake defaults:
- DVBSTREAMER5_ENABLE_VAAPI=ON
- DVBSTREAMER5_ENABLE_INTEL_QSV=ON
- DVBSTREAMER5_ENABLE_NVENC=ON

install_deps.sh automatically runs scripts/install_native_hw_backends.sh.

GPU auto-detection:
- Intel: VAAPI + iHD/i965 + oneVPL packages when available.
- NVIDIA: ubuntu-drivers install + available NVENC header packages.
- AMD: Mesa VAAPI + libdrm-amdgpu.
- Generic libva development/runtime dependencies are always installed.

No FFmpeg/GStreamer media runtime is added to DVBStreamer5.

Apply on Windows:
  powershell -ExecutionPolicy Bypass -File .\apply_stage9_v10_8_7.ps1 -RepoPath D:\PROJECTS\DVBStreamer5

After push/pull on Ubuntu:
  cd /opt/DVBStreamer5
  ./install_deps.sh
  ./scripts/hw_backend_report.sh

Clean build:
  rm -rf build-test
  cmake -S . -B build-test -DCMAKE_BUILD_TYPE=Release
  cmake --build build-test --parallel 2 --target DVBStreamer5

Note:
V10.8.6 direct VAAPI fallback should be applied first for Intel Haswell/i965.
