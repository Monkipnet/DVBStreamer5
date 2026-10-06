from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected 1 occurrence, got {count}")
    return text.replace(old, new, 1)


# Version.
p = Path("src/AppVersion.h")
s = p.read_text()
s = replace_once(s, 'kProgramVersion = "10.8.109"', 'kProgramVersion = "10.8.110"', "version")
p.write_text(s)

# Public configuration model: list of IPv4 interface addresses with filtering enabled.
p = Path("src/ConfigManager.h")
s = p.read_text()
s = replace_once(
    s,
    '    // Opt-in SRT profile for VPS/VDS/container hosts.\n'
    '    bool srtVpsVdsOptimization = false;\n'
    '    std::vector<StreamConfig> streams;',
    '    // Opt-in SRT profile for VPS/VDS/container hosts.\n'
    '    bool srtVpsVdsOptimization = false;\n'
    '    // V10.8.110: IPv4 addresses of interfaces where UDP/RTP ingress filtering is enabled.\n'
    '    // Empty by default: upgrades cannot unexpectedly alter host packet filtering.\n'
    '    std::vector<std::string> udpInputFilterInterfaces;\n'
    '    std::vector<StreamConfig> streams;',
    "AppConfig UDP filter field",
)
p.write_text(s)

# Persistence and automatic rebuild after any stream/config save.
p = Path("src/ConfigManager.cpp")
s = p.read_text()
s = replace_once(
    s,
    '#include "ConfigManager.h"\n#include "utils.h"\n',
    '#include "ConfigManager.h"\n#include "UdpInputFirewall.h"\n#include "utils.h"\n',
    "ConfigManager firewall include",
)
s = replace_once(
    s,
    '    root["telegram_chat_id"] = telegramChatId;\n'
    '    root["srt_vps_vds_optimization"] = srtVpsVdsOptimization;\n'
    '    Json::Value camClientsJson(Json::arrayValue);',
    '    root["telegram_chat_id"] = telegramChatId;\n'
    '    root["srt_vps_vds_optimization"] = srtVpsVdsOptimization;\n'
    '    Json::Value udpFilterInterfaces(Json::arrayValue);\n'
    '    for (const auto& address : udpInputFilterInterfaces) {\n'
    '        if (!address.empty()) udpFilterInterfaces.append(address);\n'
    '    }\n'
    '    root["udp_input_filter_interfaces"] = udpFilterInterfaces;\n'
    '    Json::Value camClientsJson(Json::arrayValue);',
    "serialize UDP filter interfaces",
)
s = replace_once(
    s,
    '    config.telegramToken = root.get("telegram_token", "").asString();\n'
    '    config.telegramChatId = root.get("telegram_chat_id", "").asString();\n'
    '    config.srtVpsVdsOptimization = root.get("srt_vps_vds_optimization", false).asBool();\n'
    '    if (root.isMember("cam_clients") && root["cam_clients"].isArray()) {',
    '    config.telegramToken = root.get("telegram_token", "").asString();\n'
    '    config.telegramChatId = root.get("telegram_chat_id", "").asString();\n'
    '    config.srtVpsVdsOptimization = root.get("srt_vps_vds_optimization", false).asBool();\n'
    '    if (root.isMember("udp_input_filter_interfaces") && root["udp_input_filter_interfaces"].isArray()) {\n'
    '        std::set<std::string> uniqueAddresses;\n'
    '        for (const auto& item : root["udp_input_filter_interfaces"]) {\n'
    '            if (!item.isString()) continue;\n'
    '            const std::string address = item.asString();\n'
    '            if (!address.empty() && uniqueAddresses.insert(address).second) {\n'
    '                config.udpInputFilterInterfaces.push_back(address);\n'
    '            }\n'
    '        }\n'
    '    }\n'
    '    if (root.isMember("cam_clients") && root["cam_clients"].isArray()) {',
    "deserialize UDP filter interfaces",
)
s = replace_once(
    s,
    '    if (!writeJsonAtomic(configPath, root, error)) {\n'
    '        std::cerr << "Unable to write config file " << configPath << ": "\n'
    '                  << error << std::endl;\n'
    '        return false;\n'
    '    }\n'
    '    return true;\n'
    '}',
    '    if (!writeJsonAtomic(configPath, root, error)) {\n'
    '        std::cerr << "Unable to write config file " << configPath << ": "\n'
    '                  << error << std::endl;\n'
    '        return false;\n'
    '    }\n'
    '    std::string firewallError;\n'
    '    if (!dvbstreamer5::network::UdpInputFirewall::apply(config, firewallError)) {\n'
    '        // Configuration remains valid; packet filtering deliberately fails open.\n'
    '        std::cerr << "UDP INPUT FILTER rebuild failed (fail-open): "\n'
    '                  << firewallError << std::endl;\n'
    '    }\n'
    '    return true;\n'
    '}',
    "ConfigManager save firewall rebuild",
)
p.write_text(s)

# Startup rebuild before media sockets are opened.
p = Path("src/main.cpp")
s = p.read_text()
s = replace_once(
    s,
    '#include "ConfigManager.h"\n#include "CardManager.h"\n',
    '#include "ConfigManager.h"\n#include "UdpInputFirewall.h"\n#include "CardManager.h"\n',
    "main firewall include",
)
s = replace_once(
    s,
    '    std::cerr << "Config loaded: http_port=" << configManager.config.httpPort\n'
    '              << " login=" << configManager.config.login << std::endl;\n\n'
    '    CardManager::instance().configure(configManager.config.camClients);',
    '    std::cerr << "Config loaded: http_port=" << configManager.config.httpPort\n'
    '              << " login=" << configManager.config.login << std::endl;\n\n'
    '    std::string udpFilterError;\n'
    '    if (!dvbstreamer5::network::UdpInputFirewall::apply(configManager.config, udpFilterError)) {\n'
    '        std::cerr << "UDP INPUT FILTER startup apply failed (fail-open): "\n'
    '                  << udpFilterError << std::endl;\n'
    '    }\n\n'
    '    CardManager::instance().configure(configManager.config.camClients);',
    "startup firewall apply",
)
p.write_text(s)

# HTTP API + Network modal.
p = Path("src/HttpServer.cpp")
s = p.read_text()
s = replace_once(
    s,
    '#include "OscamMiniManager.h"\n#include "protocols/SrtVpsProfile.h"\n',
    '#include "OscamMiniManager.h"\n#include "UdpInputFirewall.h"\n#include "protocols/SrtVpsProfile.h"\n',
    "HttpServer firewall include",
)
s = replace_once(
    s,
    '      item["address"] = address == interfaceAddresses.end() ? "" : address->address;\n'
    '      item["rx_mbps"] = rxMbps;\n'
    '      item["tx_mbps"] = txMbps;',
    '      item["address"] = address == interfaceAddresses.end() ? "" : address->address;\n'
    '      item["udp_input_filter"] = address != interfaceAddresses.end() &&\n'
    '          dvbstreamer5::network::UdpInputFirewall::enabledFor(\n'
    '              configManager.config, address->address);\n'
    '      item["rx_mbps"] = rxMbps;\n'
    '      item["tx_mbps"] = txMbps;',
    "network metrics UDP filter state",
)

# POST endpoint. It pre-applies the candidate ruleset; persistence happens only
# after nftables accepted it. On save failure the old in-memory ruleset is restored.
post_anchor = '''            } else if (target == "/api/oscam-mini/save") {
                res.set(http::field::content_type, "application/json");
                res.body() = OscamMiniManager::instance().saveSettingsJson(req.body());'''
post_replacement = '''            } else if (target == "/api/network/udp-input-filter") {
                res.set(http::field::content_type, "application/json");
                Json::Value payload;
                Json::CharReaderBuilder builder;
                std::string parseError;
                std::istringstream input(req.body());
                Json::Value reply(Json::objectValue);
                if (req.body().size() > 4096 ||
                    !Json::parseFromStream(builder, input, &payload, &parseError) ||
                    !payload.isObject() || !payload["address"].isString() ||
                    !payload["enabled"].isBool()) {
                    res.result(http::status::bad_request);
                    reply["result"] = "error";
                    reply["error"] = "Invalid UDP input filter request";
                } else {
                    const std::string address = payload["address"].asString();
                    const bool enabled = payload["enabled"].asBool();
                    const auto interfaces = enumerateNetworkInterfaces();
                    const auto iface = std::find_if(interfaces.begin(), interfaces.end(),
                        [&address](const NetworkInterface& candidate) {
                            return candidate.address == address && candidate.name != "lo" &&
                                   candidate.address.rfind("127.", 0) != 0;
                        });
                    if (iface == interfaces.end()) {
                        res.result(http::status::bad_request);
                        reply["result"] = "error";
                        reply["error"] = "Network interface is unavailable";
                    } else {
                        const auto previous = configManager.config.udpInputFilterInterfaces;
                        auto next = previous;
                        next.erase(std::remove(next.begin(), next.end(), address), next.end());
                        if (enabled) next.push_back(address);
                        std::sort(next.begin(), next.end());
                        next.erase(std::unique(next.begin(), next.end()), next.end());
                        configManager.config.udpInputFilterInterfaces = next;

                        std::string firewallError;
                        if (!dvbstreamer5::network::UdpInputFirewall::apply(
                                configManager.config, firewallError)) {
                            configManager.config.udpInputFilterInterfaces = previous;
                            std::string rollbackError;
                            (void)dvbstreamer5::network::UdpInputFirewall::apply(
                                configManager.config, rollbackError);
                            res.result(http::status::bad_request);
                            reply["result"] = "error";
                            reply["error"] = firewallError;
                        } else if (!configManager.save()) {
                            configManager.config.udpInputFilterInterfaces = previous;
                            std::string rollbackError;
                            (void)dvbstreamer5::network::UdpInputFirewall::apply(
                                configManager.config, rollbackError);
                            res.result(http::status::internal_server_error);
                            reply["result"] = "error";
                            reply["error"] = "Unable to save configuration";
                        } else {
                            reply["result"] = "ok";
                            reply["address"] = address;
                            reply["enabled"] = enabled;
                        }
                    }
                }
                Json::StreamWriterBuilder writer;
                writer["indentation"] = "";
                res.body() = Json::writeString(writer, reply);
            } else if (target == "/api/oscam-mini/save") {
                res.set(http::field::content_type, "application/json");
                res.body() = OscamMiniManager::instance().saveSettingsJson(req.body());'''
s = replace_once(s, post_anchor, post_replacement, "UDP filter POST endpoint")

s = replace_once(
    s,
    "    networkLoad:'Network interface load', interface:'Interface', incoming:'Incoming', outgoing:'Outgoing', close:'Close',",
    "    networkLoad:'Network interface load', interface:'Interface', incoming:'Incoming', outgoing:'Outgoing', udpInputFilter:'Filter inbound UDP', close:'Close',",
    "English UDP filter translation",
)
s = replace_once(
    s,
    "    networkLoad:'Загрузка сетевых интерфейсов', interface:'Интерфейс', incoming:'Входящий', outgoing:'Исходящий', close:'Закрыть',",
    "    networkLoad:'Загрузка сетевых интерфейсов', interface:'Интерфейс', incoming:'Входящий', outgoing:'Исходящий', udpInputFilter:'Фильтровать входящий UDP', close:'Закрыть',",
    "Russian UDP filter translation",
)
s = replace_once(
    s,
    '    <table class="network-table"><thead><tr><th>${t(\'interface\')}</th><th>${t(\'incoming\')}</th><th>${t(\'outgoing\')}</th></tr></thead><tbody id="networkTableBody"></tbody></table>',
    '    <table class="network-table"><thead><tr><th>${t(\'interface\')}</th><th>${t(\'incoming\')}</th><th>${t(\'outgoing\')}</th><th>${t(\'udpInputFilter\')}</th></tr></thead><tbody id="networkTableBody"></tbody></table>',
    "Network modal fourth column",
)
old_rows = '''  table.innerHTML = interfaces.length ? interfaces.map(iface => `
    <tr><td>${iface.name}${iface.address ? ` (${iface.address})` : ''}</td><td>${Number(iface.rx_mbps || 0).toFixed(2)} Mbps</td><td>${Number(iface.tx_mbps || 0).toFixed(2)} Mbps</td></tr>
  `).join('') : `<tr><td colspan="3" class="network-empty">${t('interfacesNotFound')}</td></tr>`;
}
function fetchSystemMetrics() {'''
new_rows = '''  table.innerHTML = interfaces.length ? interfaces.map(iface => `
    <tr>
      <td>${iface.name}${iface.address ? ` (${iface.address})` : ''}</td>
      <td>${Number(iface.rx_mbps || 0).toFixed(2)} Mbps</td>
      <td>${Number(iface.tx_mbps || 0).toFixed(2)} Mbps</td>
      <td><label><input type="checkbox" data-address="${iface.address || ''}"
        ${iface.udp_input_filter ? 'checked' : ''}
        ${!iface.address || iface.name === 'lo' ? 'disabled' : ''}
        onchange="setUdpInputFilter(this)"> ${t('udpInputFilter')}</label></td>
    </tr>
  `).join('') : `<tr><td colspan="4" class="network-empty">${t('interfacesNotFound')}</td></tr>`;
}
async function setUdpInputFilter(control) {
  const address = control.dataset.address || '';
  const desired = control.checked;
  control.disabled = true;
  try {
    const response = await fetch('/api/network/udp-input-filter', {
      method: 'POST',
      headers: {'Content-Type':'application/json'},
      body: JSON.stringify({address, enabled: desired})
    });
    const result = await response.json();
    if (!response.ok || result.result !== 'ok') {
      throw new Error(result.error || `HTTP ${response.status}`);
    }
    await fetchSystemMetrics();
  } catch (error) {
    control.checked = !desired;
    uiError(error?.message || error);
  } finally {
    control.disabled = false;
  }
}
function fetchSystemMetrics() {'''
s = replace_once(s, old_rows, new_rows, "Network modal UDP filter controls")
p.write_text(s)

# Optional runtime dependency in the portable installer. Existing installations
# without the feature enabled remain usable even if package installation is unavailable.
p = Path("scripts/make_portable_linux_bundle.sh")
s = p.read_text()
s = replace_once(
    s,
    'command -v systemctl >/dev/null 2>&1 || fail "systemd/systemctl is required"\n'
    '[[ -x "$BUNDLE_DIR/run.sh" && -x "$BUNDLE_DIR/bin/$APP" ]] || fail "bundle is incomplete"',
    'command -v systemctl >/dev/null 2>&1 || fail "systemd/systemctl is required"\n'
    'if ! command -v nft >/dev/null 2>&1; then\n'
    '    if command -v apt-get >/dev/null 2>&1; then\n'
    '        log "Installing nftables for optional per-interface UDP input filtering"\n'
    '        apt-get update\n'
    '        DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends nftables || true\n'
    '    fi\n'
    'fi\n'
    'if ! command -v nft >/dev/null 2>&1; then\n'
    '    echo "WARNING: nft is unavailable; UDP input filtering will stay fail-open." >&2\n'
    'fi\n'
    '[[ -x "$BUNDLE_DIR/run.sh" && -x "$BUNDLE_DIR/bin/$APP" ]] || fail "bundle is incomplete"',
    "portable nftables dependency",
)
p.write_text(s)

# Header compile hygiene; file itself was added separately so it remains easy to audit.
p = Path("src/UdpInputFirewall.h")
s = p.read_text()
s = replace_once(s, '#include <arpa/inet.h>\n#include <cstdio>\n', '#include <arpa/inet.h>\n#include <cctype>\n#include <cstdio>\n', "firewall cctype include")
p.write_text(s)
