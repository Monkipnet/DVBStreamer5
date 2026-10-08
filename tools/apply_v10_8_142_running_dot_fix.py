from pathlib import Path


def replace_once(path: str, old: str, new: str) -> None:
    p = Path(path)
    text = p.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{path}: expected one match, got {count}")
    p.write_text(text.replace(old, new, 1), encoding="utf-8")


replace_once(
    "src/AppVersion.h",
    'inline constexpr const char* kProgramVersion = "10.8.141";',
    'inline constexpr const char* kProgramVersion = "10.8.142";',
)

# The stream runtime status can be returned as Russian "Работает" and only
# later localized to "Running" by the UI observer. Recognize both raw forms
# so the visible main stream tile uses the green dot before localization.
replace_once(
    "src/HttpServer.cpp",
    "function tileRuntimeStatusIsRunning(value) {\n  return /^running$/i.test(tileRuntimeStatus(value));\n}",
    "function tileRuntimeStatusIsRunning(value) {\n  const status = tileRuntimeStatus(value).toLowerCase();\n  return status === 'running' || status === 'работает';\n}",
)

# The visible Running reported by the user is the active MPTS state. It is
# rendered as Russian "Работает" and then localized to "Running". Keep the
# inactive text state, but render active as a compact green status dot.
replace_once(
    "src/HttpServer.cpp",
    ".mpts-state.on{color:#9ef3bd;background:rgba(34,197,94,.14);border:1px solid rgba(34,197,94,.38)}",
    ".mpts-state.on{width:10px;height:10px;min-width:10px;padding:0;border-radius:50%;font-size:0;color:transparent;background:#22c55e;border:0;box-shadow:0 0 0 2px rgba(34,197,94,.18)}",
)

replace_once(
    "src/HttpServer.cpp",
    '      <span class="mpts-state ${active?\'on\':\'off\'}">${active?\'Работает\':\'Остановлен\'}</span>',
    '      <span class="mpts-state ${active?\'on\':\'off\'}" title="${active?\'Работает\':\'Остановлен\'}" aria-label="${active?\'Работает\':\'Остановлен\'}">${active?\'\':\'Остановлен\'}</span>',
)
