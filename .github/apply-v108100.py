from pathlib import Path


def replace_once(path: Path, old: str, new: str) -> None:
    text = path.read_text()
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{path}: expected exactly one match, found {count}")
    path.write_text(text.replace(old, new, 1))


replace_once(
    Path("src/AppVersion.h"),
    'inline constexpr const char* kProgramVersion = "10.8.99";',
    'inline constexpr const char* kProgramVersion = "10.8.100";'
)

replace_once(
    Path("src/HttpServer.cpp"),
    '''void HttpServer::refreshHttpPorts() {
    boost::asio::post(ioContext, [this]() {
        bindHttpPorts(configuredHttpPorts());
    });
}
''',
    '''void HttpServer::refreshHttpPorts() {
    // V10.8.100: config saves are allowed to be partial and the web UI may
    // issue several of them in quick succession. Rebinding an unchanged
    // listener set closes healthy acceptors and can race the next save into
    // EADDRINUSE. Keep the existing listeners when every requested port is
    // already open; bindHttpPorts() remains the recovery/reconfiguration path
    // when the set really changed or a listener disappeared.
    const auto desiredPorts = configuredHttpPorts();
    boost::asio::post(ioContext, [this, desiredPorts]() {
        {
            std::lock_guard<std::mutex> acceptorLock(acceptorsMutex);
            bool unchanged = acceptors.size() == desiredPorts.size();
            if (unchanged) {
                for (int port : desiredPorts) {
                    const auto found = acceptors.find(port);
                    if (found == acceptors.end() || !found->second || !found->second->is_open()) {
                        unchanged = false;
                        break;
                    }
                }
            }
            if (unchanged) return;
        }
        bindHttpPorts(desiredPorts);
    });
}
'''
)

replace_once(
    Path("src/HttpServer.cpp"),
    '''    configManager.config = nextConfig;
    configManager.save();
    CardManager::instance().configure(configManager.config.camClients);
    refreshHttpPorts();
''',
    '''    bool camClientsChanged = previousConfig.camClients.size() != nextConfig.camClients.size();
    if (!camClientsChanged) {
        for (std::size_t index = 0; index < previousConfig.camClients.size(); ++index) {
            if (previousConfig.camClients[index].toJson() != nextConfig.camClients[index].toJson()) {
                camClientsChanged = true;
                break;
            }
        }
    }

    configManager.config = nextConfig;
    configManager.save();
    // V10.8.100: CardManager::configure() synchronizes OSCam-mini and may
    // restart its systemd service when listener configuration changes. A
    // stream-only/partial config save must not restart OSCam or reload the CA
    // plugin when the CAM client configuration is byte-for-byte unchanged.
    if (camClientsChanged) {
        CardManager::instance().configure(configManager.config.camClients);
    }
    refreshHttpPorts();
'''
)

print("V10.8.100 patch applied")
