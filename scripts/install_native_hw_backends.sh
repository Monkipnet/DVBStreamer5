#!/usr/bin/env bash
set -euo pipefail

export DEBIAN_FRONTEND=noninteractive

SUDO=()
if [[ ${EUID:-$(id -u)} -ne 0 ]]; then
  command -v sudo >/dev/null 2>&1 || {
    echo "ERROR: run as root or install sudo." >&2
    exit 1
  }
  SUDO=(sudo)
fi

APT=("${SUDO[@]}" apt-get)
NO_DRIVER_INSTALL=0
if [[ "${1:-}" == "--no-driver-install" ]]; then
  NO_DRIVER_INSTALL=1
fi

pkg_exists() {
  apt-cache show "$1" >/dev/null 2>&1
}

install_if_available() {
  local pkgs=()
  local p
  for p in "$@"; do
    if pkg_exists "$p"; then
      pkgs+=("$p")
    fi
  done
  if ((${#pkgs[@]})); then
    "${APT[@]}" install -y --no-install-recommends "${pkgs[@]}"
  fi
}

echo "=== DVBStreamer5 native hardware backend installer ==="

"${APT[@]}" update
install_if_available pciutils lsb-release ca-certificates curl gnupg

GPU_TEXT=""
if command -v lspci >/dev/null 2>&1; then
  GPU_TEXT="$(lspci -nn | grep -Ei 'VGA compatible controller|3D controller|Display controller' || true)"
fi

echo
echo "Detected display controllers:"
if [[ -n "$GPU_TEXT" ]]; then
  echo "$GPU_TEXT"
else
  echo "  no PCI display controller found (container/VM/passthrough may hide PCI identity)"
fi

HAS_INTEL=0
HAS_NVIDIA=0
HAS_AMD=0
grep -qi 'Intel' <<<"$GPU_TEXT" && HAS_INTEL=1 || true
grep -Eqi 'NVIDIA|GeForce|Quadro|Tesla' <<<"$GPU_TEXT" && HAS_NVIDIA=1 || true
grep -Eqi 'AMD|ATI|Advanced Micro Devices' <<<"$GPU_TEXT" && HAS_AMD=1 || true

if compgen -G "/sys/class/drm/card*/device/vendor" >/dev/null; then
  for vf in /sys/class/drm/card*/device/vendor; do
    [[ -r "$vf" ]] || continue
    vendor="$(tr '[:upper:]' '[:lower:]' < "$vf")"
    case "$vendor" in
      0x8086) HAS_INTEL=1 ;;
      0x10de) HAS_NVIDIA=1 ;;
      0x1002|0x1022) HAS_AMD=1 ;;
    esac
  done
fi

echo
echo "Vendor flags: Intel=$HAS_INTEL NVIDIA=$HAS_NVIDIA AMD=$HAS_AMD"

install_if_available   libva2 libva-drm2 libva-dev vainfo   libdrm2 libdrm-dev pkg-config

if ((HAS_INTEL)); then
  echo
  echo "=== Intel GPU: installing VAAPI + oneVPL/QSV support ==="
  # intel-media-va-driver and intel-media-va-driver-non-free conflict.
  # Keep an installed variant; otherwise prefer non-free when available.
  if dpkg-query -W -f='${Status}' intel-media-va-driver-non-free 2>/dev/null | grep -q 'install ok installed'; then
    echo "Intel VAAPI driver: keeping installed intel-media-va-driver-non-free"
  elif dpkg-query -W -f='${Status}' intel-media-va-driver 2>/dev/null | grep -q 'install ok installed'; then
    echo "Intel VAAPI driver: keeping installed intel-media-va-driver"
  elif apt-cache show intel-media-va-driver-non-free >/dev/null 2>&1; then
    install_if_available intel-media-va-driver-non-free
  else
    install_if_available intel-media-va-driver
  fi

  install_if_available i965-va-driver libvpl2 libvpl-dev libmfx-gen1.2 libmfx-gen-dev onevpl-tools
fi

if ((HAS_AMD)); then
  echo
  echo "=== AMD GPU: installing Mesa VAAPI support ==="
  install_if_available     mesa-va-drivers     mesa-vulkan-drivers     libdrm-amdgpu1
fi

if ((HAS_NVIDIA)); then
  echo
  echo "=== NVIDIA GPU: installing proprietary driver/runtime ==="
  install_if_available ubuntu-drivers-common

  if ((NO_DRIVER_INSTALL == 0)); then
    if command -v ubuntu-drivers >/dev/null 2>&1; then
      echo "Running: ubuntu-drivers install"
      "${SUDO[@]}" ubuntu-drivers install ||         echo "WARNING: ubuntu-drivers install failed; NVENC may remain unavailable." >&2
    else
      echo "WARNING: ubuntu-drivers is unavailable on this distribution." >&2
    fi
  else
    echo "Driver installation skipped by --no-driver-install."
  fi

  install_if_available     libffmpeg-nvenc-dev     nv-codec-headers     nvidia-cuda-dev
fi

TARGET_USER="${SUDO_USER:-${USER:-root}}"
if [[ -n "$TARGET_USER" && "$TARGET_USER" != "root" ]] && id "$TARGET_USER" >/dev/null 2>&1; then
  for grp in video render; do
    if getent group "$grp" >/dev/null 2>&1; then
      "${SUDO[@]}" usermod -aG "$grp" "$TARGET_USER" || true
    fi
  done
fi

echo
echo "=== Runtime diagnostics ==="
ls -l /dev/dri 2>/dev/null || true

if command -v vainfo >/dev/null 2>&1 && [[ -e /dev/dri/renderD128 ]]; then
  echo
  echo "--- vainfo /dev/dri/renderD128 ---"
  vainfo --display drm --device /dev/dri/renderD128 2>&1 |     grep -E 'Driver version|VAProfile|EntrypointEnc|error|failed' || true
fi

if command -v vpl-inspect >/dev/null 2>&1; then
  echo
  echo "--- oneVPL implementations ---"
  vpl-inspect 2>&1 | head -n 120 || true
fi

if command -v nvidia-smi >/dev/null 2>&1; then
  echo
  echo "--- NVIDIA ---"
  nvidia-smi --query-gpu=name,driver_version --format=csv,noheader 2>/dev/null || true
fi

echo
echo "=== Build prerequisite summary ==="
for h in   /usr/include/vpl/mfxdispatcher.h   /usr/include/va/va.h   /usr/include/ffnvcodec/nvEncodeAPI.h   /usr/include/nvEncodeAPI.h   /usr/include/ffnvcodec/dynlink_cuda.h   /usr/include/ffnvcodec/dynlink_cuviddec.h   /usr/include/ffnvcodec/dynlink_nvcuvid.h
do
  [[ -e "$h" ]] && echo "FOUND  $h"
done

ldconfig -p 2>/dev/null |   grep -E 'libvpl|libva\.so|libva-drm|libnvidia-encode|libnvcuvid|libcuda' || true

echo
echo "Hardware dependency installation finished."
if ((HAS_NVIDIA && NO_DRIVER_INSTALL == 0)); then
  echo "NOTE: after first NVIDIA driver installation a reboot may be required."
fi
echo "Re-run CMake from a clean build directory so all detected backends are enabled."
