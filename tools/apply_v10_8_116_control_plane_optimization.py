from pathlib import Path


def replace_once(path: str, old: str, new: str) -> None:
    p = Path(path)
    text = p.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{path}: expected one match, got {count}\n--- OLD ---\n{old}")
    p.write_text(text.replace(old, new, 1), encoding="utf-8")


# Guard the dead-code removals: abort if either legacy symbol is used outside
# its own implementation/header before deleting it.
references = []
for path in [Path("CMakeLists.txt"), *Path("src").glob("*.h"), *Path("src").glob("*.cpp")]:
    if path.name in {"HttpServer.h", "HttpServer.cpp", "NewcamdStatusBackend.h", "NewcamdStatusBackend.cpp"}:
        continue
    text = path.read_text(encoding="utf-8", errors="ignore")
    if "addEndpoint(" in text or "NewcamdStatusBackend" in text:
        references.append(str(path))
if references:
    raise SystemExit("dead-code guard found external references: " + ", ".join(references))

replace_once(
    "src/AppVersion.h",
    'inline constexpr const char* kProgramVersion = "10.8.115";',
    'inline constexpr const char* kProgramVersion = "10.8.116";',
)

replace_once(
    "src/HttpServer.h",
    '#include <thread>\n#include <functional>\n#include <unordered_map>\n',
    '#include <thread>\n#include <unordered_map>\n',
)
replace_once(
    "src/HttpServer.h",
    '    bool start();\n    void addEndpoint(const std::string& path, std::function<void(const boost::asio::ip::tcp::socket&)> handler);\n\nprivate:\n',
    '    bool start();\n\nprivate:\n',
)
replace_once(
    "src/HttpServer.h",
    '    int64_t qualityLastPersist = 0;\n    std::unordered_map<std::string, std::function<void(const boost::asio::ip::tcp::socket&)>> endpointHandlers;\n};\n',
    '    int64_t qualityLastPersist = 0;\n};\n',
)

replace_once(
    "src/HttpServer.cpp",
    '''void HttpServer::addEndpoint(const std::string& path, std::function<void(const boost::asio::ip::tcp::socket&)> handler) {
    // Store endpoint handler for future use
    // This is a simple implementation - in a real server you'd want proper routing
    endpointHandlers[path] = handler;
}

''',
    '',
)

replace_once(
    "src/HttpServer.cpp",
    '''async function statePollLoop() {
  await fetchState();
  clearTimeout(statePollTimer);
  statePollTimer = setTimeout(statePollLoop, 2000);
}
''',
    '''async function statePollLoop() {
  // V10.8.116: a background browser tab must not keep forcing large /api/state
  // JSON snapshots. Quality history is sampled server-side independently.
  if (!document.hidden) await fetchState();
  clearTimeout(statePollTimer);
  statePollTimer = setTimeout(statePollLoop, document.hidden ? 15000 : 2000);
}
''',
)
replace_once(
    "src/HttpServer.cpp",
    '''async function metricsPollLoop() {
  await fetchSystemMetrics();
  clearTimeout(metricsPollTimer);
  metricsPollTimer = setTimeout(metricsPollLoop, 3000);
}
''',
    '''async function metricsPollLoop() {
  // Avoid /proc parsing, JSON construction and allocator churn for hidden tabs.
  if (!document.hidden) await fetchSystemMetrics();
  clearTimeout(metricsPollTimer);
  metricsPollTimer = setTimeout(metricsPollLoop, document.hidden ? 15000 : 3000);
}
''',
)
replace_once(
    "src/HttpServer.cpp",
    '''async function updateSatelliteSignal() {
  if (satelliteScanning || satelliteSignalPending || !document.getElementById('satFrequency')) return;
''',
    '''async function updateSatelliteSignal() {
  if (document.hidden || satelliteScanning || satelliteSignalPending || !document.getElementById('satFrequency')) return;
''',
)
replace_once(
    "src/HttpServer.cpp",
    '''  qualityChart.timer = setInterval(() => {
    loadQualityHistory(qualityChart.streamId, qualityChart.period);
  }, qualityChart.refreshMs);
''',
    '''  qualityChart.timer = setInterval(() => {
    if (!document.hidden) loadQualityHistory(qualityChart.streamId, qualityChart.period);
  }, qualityChart.refreshMs);
''',
)
replace_once(
    "src/HttpServer.cpp",
    '''window.onload = () => {
  applyLanguage();
  loadInterfaces();
  statePollLoop();
  metricsPollLoop();
};
window.addEventListener('beforeunload', () => {
''',
    '''window.onload = () => {
  applyLanguage();
  loadInterfaces();
  statePollLoop();
  metricsPollLoop();
};
document.addEventListener('visibilitychange', () => {
  if (document.hidden) return;
  // Refresh immediately after returning to the UI instead of waiting for the
  // long background retry interval. Promise guards collapse any overlap.
  clearTimeout(statePollTimer);
  clearTimeout(metricsPollTimer);
  statePollLoop();
  metricsPollLoop();
  if (qualityChart.streamId && qualityChart.refreshMs) {
    loadQualityHistory(qualityChart.streamId, qualityChart.period);
  }
  updateSatelliteSignal();
});
window.addEventListener('beforeunload', () => {
''',
)

# This status-only prototype has never been part of the CMake target and has no
# references in the active source tree. The real CA path is the newcamd plugin.
for dead in (Path("src/NewcamdStatusBackend.h"), Path("src/NewcamdStatusBackend.cpp")):
    if not dead.exists():
        raise SystemExit(f"expected dead legacy file is missing: {dead}")
    dead.unlink()

print("V10.8.116 control-plane optimization applied")
