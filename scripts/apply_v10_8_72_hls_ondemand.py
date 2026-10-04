#!/usr/bin/env python3
from pathlib import Path


def replace_once(path: str, old: str, new: str) -> None:
    p = Path(path)
    text = p.read_text()
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{path}: expected exactly one anchor, found {count}: {old[:160]!r}")
    p.write_text(text.replace(old, new, 1))


replace_once(
    "src/AppVersion.h",
    'inline constexpr const char* kProgramVersion = "10.8.71";',
    'inline constexpr const char* kProgramVersion = "10.8.72";')

replace_once(
    "src/StreamManager.cpp",
    '    constexpr auto kIdleGrace = std::chrono::seconds(10);',
    '    // HLS clients fetch short-lived playlist/segment resources rather than keeping\n'
    '    // a persistent socket open. Ten seconds was too aggressive and could stop an\n'
    '    // actively watched channel between requests, producing visible HLS stalls.\n'
    '    constexpr auto kIdleGrace = std::chrono::seconds(30);')

replace_once(
    "src/StreamManager.cpp",
    '                std::cerr << "ONDEMAND DEACTIVATE stream=" << id\n'
    '                          << " reason=no-clients idle_s=10" << std::endl;',
    '                std::cerr << "ONDEMAND DEACTIVATE stream=" << id\n'
    '                          << " reason=no-clients idle_s=30" << std::endl;')

replace_once(
    "src/StreamManager.cpp",
    '''bool StreamManager::addStreamSession(const std::string& streamId, const std::string& clientIp,
                                     const std::string& protocol) {
    if (streamId.empty() || clientIp.empty()) return false;
    std::lock_guard<std::mutex> lock(managerMutex);
    const std::string ip = normalizeIpAddress(clientIp);
    const std::string key = protocol + ":" + streamId + ":" + ip + ":" +
        std::to_string(nextSessionId.fetch_add(1));
    adHocSessions[key] = {streamId, ip, protocol, std::chrono::steady_clock::now(), -1, {}};
    return true;
}
''',
    '''bool StreamManager::addStreamSession(const std::string& streamId, const std::string& clientIp,
                                     const std::string& protocol) {
    if (streamId.empty() || clientIp.empty()) return false;
    std::lock_guard<std::mutex> lock(managerMutex);
    const std::string ip = normalizeIpAddress(clientIp);
    // HLS performs many independent HTTP requests per viewer. Reuse one logical
    // session per protocol/stream/client and only refresh its activity timestamp;
    // otherwise every .m3u8/.ts request allocates another map node for two minutes.
    const std::string key = protocol + ":" + streamId + ":" + ip;
    auto& session = adHocSessions[key];
    session.streamId = streamId;
    session.clientIp = ip;
    session.protocol = protocol;
    session.lastActivity = std::chrono::steady_clock::now();
    session.upstreamFd = -1;
    session.previewSession.clear();
    return true;
}
''')

checks = {
    "src/AppVersion.h": ['kProgramVersion = "10.8.72"'],
    "src/StreamManager.cpp": [
        'kIdleGrace = std::chrono::seconds(30)',
        'reason=no-clients idle_s=30',
        'const std::string key = protocol + ":" + streamId + ":" + ip;',
        'session.lastActivity = std::chrono::steady_clock::now();',
    ],
}
for path, needles in checks.items():
    text = Path(path).read_text()
    for needle in needles:
        if needle not in text:
            raise SystemExit(f"{path}: missing marker {needle!r}")

print("V10.8.72 HLS OnDemand patch applied")
