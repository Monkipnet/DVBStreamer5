#!/usr/bin/env bash
set -u

echo "=== DVBStreamer5 hardware backend report ==="
echo
echo "[PCI]"
lspci -nn 2>/dev/null | grep -Ei 'VGA compatible controller|3D controller|Display controller' || true

echo
echo "[DRM]"
ls -la /dev/dri 2>/dev/null || true

echo
echo "[VAAPI]"
if command -v vainfo >/dev/null 2>&1; then
  node="$(find /dev/dri -maxdepth 1 -name 'renderD*' 2>/dev/null | head -n1)"
  if [[ -n "$node" ]]; then
    vainfo --display drm --device "$node" 2>&1 |       grep -E 'Driver version|VAProfile|EntrypointEnc|error|failed' || true
  fi
fi

echo
echo "[oneVPL]"
if command -v vpl-inspect >/dev/null 2>&1; then
  vpl-inspect 2>&1 | head -n 160 || true
else
  echo "vpl-inspect not installed"
fi

echo
echo "[NVIDIA]"
if command -v nvidia-smi >/dev/null 2>&1; then
  nvidia-smi || true
else
  echo "nvidia-smi not installed"
fi

echo
echo "[Libraries]"
ldconfig -p 2>/dev/null |   grep -E 'libvpl|libva\.so|libva-drm|libnvidia-encode|libcuda' || true

echo
echo "[Headers]"
find /usr/include -maxdepth 3   \( -name mfxdispatcher.h -o -name va.h -o -name nvEncodeAPI.h \)   -print 2>/dev/null || true
