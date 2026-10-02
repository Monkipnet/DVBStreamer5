DVBStreamer5 Stage 9 V10.8.6 - Native Intel VAAPI Haswell fallback

Target:
- Intel Haswell / i965
- VAAPI H.264 EncSlice available
- oneVPL returns MFX_ERR_NOT_FOUND (-9)

Ubuntu:
  apt update
  apt install -y libva-dev libva-drm2 vainfo libvpl-dev

Build:
  rm -rf build-test
  cmake -S . -B build-test 2>&1 | tee /tmp/cmake-v1086.log
  grep -E "Native Intel VAAPI|Native Intel QSV" /tmp/cmake-v1086.log
  cmake --build build-test --parallel 2 --target DVBStreamer5

Expected runtime log:
  NATIVE HW ENCODER Intel oneVPL unavailable; falling back to direct VAAPI H.264
  NATIVE HW ENCODER backend=vaapi-direct codec=h264 ...

HEVC on Haswell remains CPU/Kvazaar.

GitHub push note:
The connected GitHub integration returned HTTP 403 for write operations, so this hotfix was not pushed automatically.
