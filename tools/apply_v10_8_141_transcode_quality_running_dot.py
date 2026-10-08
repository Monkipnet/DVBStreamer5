from pathlib import Path
import re


def replace_once(path: str, old: str, new: str) -> None:
    p = Path(path)
    text = p.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{path}: expected one match for {old[:80]!r}, got {count}")
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

# All NVIDIA renditions share the encoder preset independently of geometry.
# Raise the normal NVENC and NVDEC->NVENC zero-copy paths from P3 to P5 while
# keeping bitrate, low-latency tuning, GOP, timestamps and rate-control intact.
for path in (
    "src/media/NativeHardwareCodec.cpp",
    "src/media/NativeNvidiaZeroCopy.cpp",
):
    replace_all_checked(path, "NV_ENC_PRESET_P3_GUID", "NV_ENC_PRESET_P5_GUID")
    replace_word_checked(path, "p3", "p5")

# Running is only a presentation change: backend state stays untouched.
replace_once(
    "src/HttpServer.cpp",
    ".tile .runtime-status{min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;color:#9ca8bb;font-size:9px}",
    ".tile .runtime-status{min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;color:#9ca8bb;font-size:9px}.tile .runtime-status.running-dot{display:inline-block;flex:0 0 auto;width:9px;height:9px;min-width:9px;border-radius:50%;background:#22c55e;box-shadow:0 0 0 2px rgba(34,197,94,.18);overflow:visible}",
)

replace_once(
    "src/HttpServer.cpp",
    "function tileRuntimeStatus(value) {\n  return String(value ?? '')\n    .replace(/\\bnative\\s+/gi, '')\n    .replace(/\\s{2,}/g, ' ')\n    .trim();\n}",
    "function tileRuntimeStatus(value) {\n  return String(value ?? '')\n    .replace(/\\bnative\\s+/gi, '')\n    .replace(/\\s{2,}/g, ' ')\n    .trim();\n}\n\nfunction tileRuntimeStatusIsRunning(value) {\n  return /^running$/i.test(tileRuntimeStatus(value));\n}",
)

replace_once(
    "src/HttpServer.cpp",
    "    runtimeStatus.textContent = value;\n    runtimeStatus.title = value;",
    "    const running = tileRuntimeStatusIsRunning(value);\n    runtimeStatus.className = `runtime-status${running ? ' running-dot' : ''}`;\n    runtimeStatus.textContent = running ? '' : value;\n    runtimeStatus.title = value;\n    runtimeStatus.setAttribute('aria-label', value);",
)

old_initial = r'''            <span data-role="runtime-status" class="runtime-status" title="${escapeHtmlValue(tileRuntimeStatus(stream.status))}">${escapeHtmlValue(tileRuntimeStatus(stream.status))}</span>'''
new_initial = r'''            <span data-role="runtime-status" class="runtime-status${tileRuntimeStatusIsRunning(stream.status) ? ' running-dot' : ''}" title="${escapeHtmlValue(tileRuntimeStatus(stream.status))}" aria-label="${escapeHtmlValue(tileRuntimeStatus(stream.status))}">${tileRuntimeStatusIsRunning(stream.status) ? '' : escapeHtmlValue(tileRuntimeStatus(stream.status))}</span>'''
replace_once("src/HttpServer.cpp", old_initial, new_initial)
