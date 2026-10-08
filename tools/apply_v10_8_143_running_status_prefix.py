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
    'inline constexpr const char* kProgramVersion = "10.8.142";',
    'inline constexpr const char* kProgramVersion = "10.8.143";',
)

# Runtime status is descriptive, for example:
#   running (native media engine)
#   running (native media engine) + native transcoder
# tileRuntimeStatus() removes the word "native", but intentionally keeps the
# parenthesized details. Treat any status beginning with the normal running
# token as running while keeping unrelated/error states textual.
replace_once(
    "src/HttpServer.cpp",
    "function tileRuntimeStatusIsRunning(value) {\n  const status = tileRuntimeStatus(value).toLowerCase();\n  return status === 'running' || status === 'работает';\n}",
    "function tileRuntimeStatusIsRunning(value) {\n  const status = tileRuntimeStatus(value).toLowerCase();\n  return /^(?:running|работает)(?:$|\\s|\\()/.test(status);\n}",
)
