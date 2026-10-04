from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected exactly one match, got {count}")
    return text.replace(old, new, 1)


p = Path("src/OscamMiniManager.cpp")
s = p.read_text()

s = replace_once(
    s,
    '    const Settings settings = loadLocked();\n    const std::string established = serviceActive ? run("ss -Htan state established 2>/dev/null") : std::string();\n\n',
    '''    const Settings settings = loadLocked();
    // Include socket ownership so Remote Newcamd reader state is based on the
    // OSCam-mini process itself instead of an unrelated TCP connection that
    // happens to use the same port.
    const std::string established = serviceActive ? run("ss -Htanp state established 2>/dev/null") : std::string();
    const auto oscamSocketOnPort = [&](int port) {
        if (pid.empty() || port < 1 || port > 65535) return false;
        const std::string portToken = ":" + std::to_string(port);
        const std::string pidToken = "pid=" + pid + ",";
        std::istringstream sockets(established);
        std::string socketLine;
        while (std::getline(sockets, socketLine)) {
            if (socketLine.find(portToken) != std::string::npos &&
                socketLine.find(pidToken) != std::string::npos) {
                return true;
            }
        }
        return false;
    };

''',
    "owned established sockets",
)

s = replace_once(
    s,
    '''            const std::string portToken = ":" + std::to_string(user.port);
            if (established.find(portToken) != std::string::npos) {
                state = "active";
                detail = "Newcamd клиент подключён";
            } else {
''',
    '''            if (oscamSocketOnPort(user.port)) {
                state = "active";
                detail = "Newcamd клиент подключён";
            } else {
''',
    "downstream newcamd socket ownership",
)

old_reader_status = '''        } else if (serviceActive) {
            const std::string readyNeedle = reader.label + " [irdeto] ready for requests";
            const std::string readyMouseNeedle = reader.label + " [mouse] ready for requests";
            const bool ready = log.find(readyNeedle) != std::string::npos
                || log.find(readyMouseNeedle) != std::string::npos
                || log.find(reader.label + " [newcamd] proxy initialized") != std::string::npos;
            const bool initializing =
                log.find(reader.label + " [mouse] card detected") != std::string::npos
                || log.find(reader.label + " [mouse] detect irdeto card") != std::string::npos
                || log.find(reader.label + " [mouse] found card system") != std::string::npos
                || log.find(reader.label + " [pcsc] card detected") != std::string::npos
                || log.find(reader.label + " [pcsc] found card system") != std::string::npos
                || log.find(reader.label + " [irdeto] THIS WAS A SUCCESSFUL START ATTEMPT") != std::string::npos
                || log.find(reader.label + " [newcamd] connecting to ") != std::string::npos;
            const bool error =
                log.find(reader.label + " [mouse] Error activating card") != std::string::npos
                || log.find(reader.label + " [mouse] ERROR") != std::string::npos
                || log.find(reader.label + " [pcsc] ERROR") != std::string::npos
                || log.find(reader.label + " [irdeto] ERROR") != std::string::npos
                || log.find(reader.label + " [mouse] card initializing error") != std::string::npos;

            if (ready) {
                state = "active";
                detail = "Карта готова";
            } else if (error) {
                state = "down";
                detail = "Ошибка ридера";
            } else if (initializing) {
                state = "idle";
                detail = "Инициализация карты";
            } else {
                state = "idle";
                detail = "Ожидание карты";
            }
        }
'''

new_reader_status = '''        } else if (serviceActive) {
            const bool remoteNewcamd = reader.protocol == "newcamd";
            const std::string remoteEndpoint = reader.remoteHost + ":" + std::to_string(reader.remotePort);
            const std::string readyNeedle = reader.label + " [irdeto] ready for requests";
            const std::string readyMouseNeedle = reader.label + " [mouse] ready for requests";
            const bool remoteSocketReady = remoteNewcamd && oscamSocketOnPort(reader.remotePort);
            const bool remoteLoggedReady = remoteNewcamd &&
                log.find(reader.label + " [newcamd] Newcamd Server: " + remoteEndpoint + " - UserID:") != std::string::npos;
            const bool ready = remoteNewcamd
                ? remoteSocketReady
                : (log.find(readyNeedle) != std::string::npos ||
                   log.find(readyMouseNeedle) != std::string::npos);
            const bool initializing = remoteNewcamd
                ? log.find(reader.label + " [newcamd] proxy " + remoteEndpoint + " newcamd52") != std::string::npos
                : (log.find(reader.label + " [mouse] card detected") != std::string::npos
                   || log.find(reader.label + " [mouse] detect irdeto card") != std::string::npos
                   || log.find(reader.label + " [mouse] found card system") != std::string::npos
                   || log.find(reader.label + " [pcsc] card detected") != std::string::npos
                   || log.find(reader.label + " [pcsc] found card system") != std::string::npos
                   || log.find(reader.label + " [irdeto] THIS WAS A SUCCESSFUL START ATTEMPT") != std::string::npos);
            const bool error = remoteNewcamd
                ? (log.find(reader.label + " [newcamd] login failed for user") != std::string::npos
                   || log.find(reader.label + " [newcamd] server does not return 14 bytes") != std::string::npos
                   || log.find(reader.label + " [newcamd] expected MSG_CLIENT_2_SERVER_LOGIN_ACK") != std::string::npos)
                : (log.find(reader.label + " [mouse] Error activating card") != std::string::npos
                   || log.find(reader.label + " [mouse] ERROR") != std::string::npos
                   || log.find(reader.label + " [pcsc] ERROR") != std::string::npos
                   || log.find(reader.label + " [irdeto] ERROR") != std::string::npos
                   || log.find(reader.label + " [mouse] card initializing error") != std::string::npos);

            if (ready) {
                state = "active";
                detail = remoteNewcamd ? "Remote Newcamd подключён" : "Карта готова";
            } else if (error) {
                state = "down";
                detail = remoteNewcamd ? "Ошибка Remote Newcamd" : "Ошибка ридера";
            } else if (initializing || remoteLoggedReady) {
                state = "idle";
                detail = remoteNewcamd ? "Подключение к Remote Newcamd" : "Инициализация карты";
            } else {
                state = "idle";
                detail = remoteNewcamd ? "Ожидание Remote Newcamd" : "Ожидание карты";
            }
        }
'''

s = replace_once(s, old_reader_status, new_reader_status, "remote reader status")
p.write_text(s)

p = Path("src/AppVersion.h")
v = p.read_text()
v = replace_once(v, 'kProgramVersion = "10.8.63"', 'kProgramVersion = "10.8.64"', "version")
p.write_text(v)

p = Path("OSCAM_MINI.md")
d = p.read_text()
marker = "## Remote Newcamd readers\n"
status_note = '''## Remote Newcamd reader status\n\nThe OSCam-mini page marks a Remote Newcamd reader green only while the OSCam-mini\nprocess owns an established TCP socket on the configured upstream port. Startup and\nauthentication errors are derived from the bundled Newcamd client log messages; local\nmouse/Phoenix/PCSC card status handling is unchanged.\n\n'''
if status_note not in d:
    if marker not in d:
        raise SystemExit("documentation marker not found")
    d = d.replace(marker, status_note + marker, 1)
p.write_text(d)
