from pathlib import Path
import re


def replace_once(path: str, old: str, new: str) -> None:
    p = Path(path)
    text = p.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{path}: expected one match, got {count}")
    p.write_text(text.replace(old, new, 1), encoding="utf-8")


def replace_all_checked(path: str, old: str, new: str) -> int:
    p = Path(path)
    text = p.read_text(encoding="utf-8")
    count = text.count(old)
    if count < 1:
        raise SystemExit(f"{path}: expected at least one match for {old!r}")
    p.write_text(text.replace(old, new), encoding="utf-8")
    return count


def replace_word_checked(path: str, old: str, new: str) -> int:
    p = Path(path)
    text = p.read_text(encoding="utf-8")
    pattern = re.compile(rf"\b{re.escape(old)}\b")
    text2, count = pattern.subn(new, text)
    if count < 1:
        raise SystemExit(f"{path}: expected at least one word match for {old!r}")
    p.write_text(text2, encoding="utf-8")
    return count


replace_once(
    "src/AppVersion.h",
    'inline constexpr const char* kProgramVersion = "10.8.140";',
    'inline constexpr const char* kProgramVersion = "10.8.141";',
)

# V10.8.141: all NVIDIA transcoded renditions use the same encoder preset,
# independent of output geometry. Move both the normal NVENC path and the
# NVDEC->NVENC zero-copy path from P3 to P5 while retaining the existing
# low-latency tuning, configured bitrate, GOP, timestamps and rate control.
for path in (
    "src/media/NativeHardwareCodec.cpp",
    "src/media/NativeNvidiaZeroCopy.cpp",
):
    replace_all_checked(path, "NV_ENC_PRESET_P3_GUID", "NV_ENC_PRESET_P5_GUID")
    replace_word_checked(path, "p3", "p5")

# Replace only the visible exact runtime state Running with a green dot.
# All non-running runtime/error/startup states continue to show their text.
replace_once(
    "src/HttpServer.cpp",
    ".tile .status-line{display:flex;align-items:center;gap:5px;min-width:0;margin-top:2px;line-height:12px}.tile .status-pill{flex:0 0 auto;padding:1px 5px;background:rgba(255,255,255,.06);color:#c9d2e4;border-radius:999px;font-size:9px;text-transform:uppercase;letter-spacing:.06em}.tile .runtime-status{min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;color:#9ca8bb;font-size:9px}",
    ".tile .status-line{display:flex;align-items:center;gap:5px;min-width:0;margin-top:2px;line-height:12px}.tile .status-pill{flex:0 0 auto;padding:1px 5px;background:rgba(255,255,255,.06);color:#c9d2e4;border-radius:999px;font-size:9px;text-transform:uppercase;letter-spacing:.06em}.tile .runtime-status{min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;color:#9ca8bb;font-size:9px}.tile .runtime-status.running-dot{display:inline-block;flex:0 0 auto;width:9px;height:9px;min-width:9px;border-radius:50%;background:#22c55e;box-shadow:0 0 0 2px rgba(34,197,94,.18);overflow:visible}",
)

replace_once(
    "src/HttpServer.cpp",
    "function tileRuntimeStatus(value) {\n  return String(value ?? '')\n    .replace(/\\bnative\\s+/gi, '')\n    .replace(/\\s{2,}/g, ' ')\n    .trim();\n}",
    "function tileRuntimeStatus(value) {\n  return String(value ?? '')\n    .replace(/\\bnative\\s+/gi, '')\n    .replace(/\\s{2,}/g, ' ')\n    .trim();\n}\n\nfunction tileRuntimeStatusIsRunning(value) {\n  return /^running$/i.test(tileRuntimeStatus(value));\n}",
)

replace_once(
    "src/HttpServer.cpp",
    "  const runtimeStatus = tile.querySelector('[data-role=\\\"runtime-status\\\"]');\n  if (runtimeStatus) {\n    const value = tileRuntimeStatus(stream.status);\n    runtimeStatus.textContent = value;\n    runtimeStatus.title = value;\n  }",
    "  const runtimeStatus = tile.querySelector('[data-role=\\\"runtime-status\\\"]');\n  if (runtimeStatus) {\n    const value = tileRuntimeStatus(stream.status);\n    const running = tileRuntimeStatusIsRunning(value);\n    runtimeStatus.className = `runtime-status${running ? ' running-dot' : ''}`;\n    runtimeStatus.textContent = running ? '' : value;\n    runtimeStatus.title = value;\n    runtimeStatus.setAttribute('aria-label', value);\n  }",
)

replace_once(
    "src/HttpServer.cpp",
    "            <span data-role=\\\"runtime-status\\\" class=\\\"runtime-status\\\" title=\\\"${escapeHtmlValue(tileRuntimeStatus(stream.status))}\\\">${escapeHtmlValue(tileRuntimeStatus(stream.status))}</span>",
    "            <span data-role=\\\"runtime-status\\\" class=\\\"runtime-status${tileRuntimeStatusIsRunning(stream.status) ? ' running-dot' : ''}\\\" title=\\\"${escapeHtmlValue(tileRuntimeStatus(stream.status))}\\\" aria-label=\\\"${escapeHtmlValue(tileRuntimeStatus(stream.status))}\\\">${tileRuntimeStatusIsRunning(stream.status) ? '' : escapeHtmlValue(tileRuntimeStatus(stream.status))}</span>",
)
